# Splatoon 3 online support on Citron Nextendo

This document is for contributors debugging Splatoon 3 against Nextendo Network. It explains why
the game is different from the older NEX-based titles and gives a repeatable setup and diagnosis
path.

## The important distinction

Mario Kart 8 Deluxe, Splatoon 2, and similar titles use Nintendo's NEX transport. In this fork,
that normally means PRUDP over a WebSocket upgrade and TLS ALPN `http/1.1`.

Splatoon 3 uses NPLN instead:

- gRPC over HTTP/2, not a WebSocket upgrade;
- its own TLS/gRPC client, which is stricter than the older NEX clients;
- multiple control-plane connections, including a session endpoint that may use
  `gs.nintendo.net` rather than an `npln` hostname;
- peer-to-peer traffic after matchmaking, which must not be redirected to the Nextendo server;
- BCAT and friends IPC calls during startup, before the player reaches a lobby.

Changing only DNS redirection or certificate validation is therefore insufficient.

## The Windows failure fixed here

Citron's OpenSSL backend already selected the game's requested ALPN for NPLN hosts. The Windows
Schannel backend previously advertised only `http/1.1` for every connection. That is correct for
NEX, but it makes a Splatoon 3 gRPC connection fail after TCP/TLS appears to succeed: the server
cannot accept the HTTP/2 preface on the HTTP/1.1 connection.

The Schannel backend now follows the same policy as OpenSSL:

- NPLN and `gs.nintendo.net` hosts use the filtered ALPN list supplied by the game;
- all other hosts, including NEX, use `http/1.1` only;
- malformed or empty ALPN requests fall back safely to `http/1.1`.

The change is in `src/core/hle/service/ssl/ssl_backend_schannel.cpp`.

## Required emulator pieces

Keep these pieces together when porting or reviewing the implementation:

1. `src/core/loader/nextendo_s3_patches.cpp` applies the built-in, build-ID-specific Splatoon 3
   patches. One bypasses the game's certificate pinning and the other fixes the peer hostname.
   A disk mod is not a replacement: Splatoon 3 refuses to boot with mods enabled.
2. `src/core/hle/service/sockets/sfdnsres.cpp` redirects Nintendo hostnames, preserves the real
   canonical hostname, handles literal IPs without reverse lookup, and records the redirected IP
   by service port.
3. `src/core/hle/service/sockets/bsd.cpp` recovers an address lost by the game's gRPC resolver,
   rejects unsupported IPv6 sockaddr layouts with `EAFNOSUPPORT`, answers the socket options that
   gRPC checks, and implements `sendmmsg`/`recvmmsg`.
4. The deferred poll path in `bsd.cpp`, `sockets.cpp`, and the service loop must preserve the
   guest poll snapshot, support eventfd polls with no requested events, and wake frequently enough
   for HTTP/2. Returning `EINVAL` for that eventfd poll leaves the game on its loading screen.
5. `src/core/hle/service/ssl/ssl_backend_openssl.cpp` and
   `src/core/hle/service/ssl/ssl_backend_schannel.cpp` must agree on the NPLN/NEX ALPN policy.
6. The `ssl:s` command table must expose the certificate-buffer calls used by Splatoon 3. Missing
   commands here stop the game's TLS setup before the first useful network request.
7. BCAT cache creation/synchronization and title-aware friends handling are startup dependencies,
   not optional UI polish.

Do not redirect an unknown peer address or peer port to the Nextendo server. The control-plane
redirect is known from DNS; a P2P peer address is not.

## Build setup

The Linux build script uses CPM for dependencies, so a plain clone is sufficient. Submodules are
still useful for the full CI matrix, but are not required by the documented CPM Linux path.

```bash
git clone https://github.com/NextendoNetwork/citron-nextendo.git
cd citron-nextendo
./build-citron-linux.sh setup
./build-citron-linux.sh use --pgo none --lto none --nopackage
```

The development binary is `build/use-nopgo/bin/citron`. For a normal release-style local build,
use `--lto full` instead. Windows contributors should use the repository's native clang-cl path:

```bash
./build-clangtron-windows.sh setup --compiler clang-cl
./build-clangtron-windows.sh use --compiler clang-cl --pgo-type none --lto none
```

The Windows output is `build/clang-cl/use/citron.exe`. The Schannel change in this document is
specifically important for that default Windows backend.

The build environment must provide a legal dump of the game, the required system files, the
correct game update, and any required Nextendo account/server configuration. This repository does
not provide Nintendo content.

## Runtime checklist

1. Start from a clean Citron configuration or confirm that Network Redirection is enabled.
2. Sign in to Nextendo, then launch Splatoon 3 with its update installed and active.
3. Enable the log categories recommended by the main README:
   `*:Info Service:Debug Service.SSL:Debug WebService:Debug`.
4. Confirm a loader line reports a known built-in NPLN patch for the `main` module.
5. Confirm DNS redirection logs show the NPLN hostname and the configured Nextendo server IP.
6. On Windows, confirm the new Schannel log reports `npln=true` for NPLN or
   `gs.nintendo.net` and a non-empty ALPN list.
7. Only after control-plane connectivity works, debug NAT/P2P station addresses. A P2P failure is
   a different problem from the HTTP/2 control-plane failure.

The known Splatoon 3 `main` build IDs currently covered by the built-in patches are:

| Build ID | Patch set |
| --- | --- |
| `6830B3A12406CB4716FEC5ADDC35D3E2DC92D212` | certificate + peer hostname |
| `726D2B882DD9EF10F4A9D73EED088740630FB6C8` | certificate |
| `28C4287AEE36F7499DA60F3E68B54C70DA382D75` | certificate + peer hostname |

If the log says `AUCUN correctif integre pour ce build`, the most common cause is that the update's
ExeFS was not selected and the base executable is running. Do not add another network workaround
until the actual running build ID is checked. A new game update requires a fresh, verified patch
entry before online testing can be meaningful.

## Failure signatures

| Symptom | Likely cause |
| --- | --- |
| TLS completes, then Splatoon 3 reports `2321-4992` | certificate/peer patch mismatch, wrong build, or HTTP/2/ALPN failure |
| No ClientHello reaches the server | DNS/address recovery or socket connect/poll failure |
| HTTP/2 preface is sent but no useful response follows | wrong ALPN, wrong `:authority`, or corrupted canonical hostname |
| Game remains on the ink/loading screen | deferred poll/eventfd handling, `sendmmsg`, or `recvmmsg` gap |
| Friends list is empty but the lobby loads | NSA/PID identifier mapping or friends IPC handling |
| One private match works and the next does not | incorrect cross-service address fallback or NAT/P2P rewrite |
| Windows only fails while Linux works | compare Schannel and OpenSSL ALPN/SNI behavior first |

## Acceptance criteria for a port

A port is not complete when it merely reaches the title screen. It should demonstrate, with logs and
two clients where possible:

- known-build patches applied to the active `main` NSO;
- control-plane DNS, TCP, TLS, ALPN, and gRPC traffic completing;
- lobby/friend data loading;
- private matchmaking and the session transport completing;
- P2P/NAT traffic using peer addresses rather than the server redirect;
- NEX titles still negotiating HTTP/1.1 and remaining functional.

When reporting a failure, include the Citron version/commit, game version, active build ID, host OS,
the first failing signature above, and a redacted log. Never include the Nextendo session token or
unredacted peer IP addresses.

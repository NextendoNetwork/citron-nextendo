# Citron Neo — Nextendo Network / NZ:P Edition

A fork of the [Citron Neo](https://github.com/citron-neo/emulator) with two
purposes:

1. **Nextendo Network online play** — connect a Nextendo Network account and play supported titles
   online from Citron, without hosts-file edits, external DNS, or manual SSL bypass.
2. **Nazi Zombies: Portable (Emulator Edition)** — the emulator-side fixes this fork was
   originally created for, and still carries.

> [!WARNING]
> **This is a work in progress. Expect bugs.**
>
> Online support is new, incomplete, and only lightly tested — largely by one person, on one
> machine, against multiple games. Things will break. If you hit a problem, please
> **[open an issue](../../issues)** and include:
>
> - your `citron_log.txt` (Linux: `~/.local/share/citron/log/citron_log.txt`)
> - the exact error code the game showed, if any (e.g. `2306-0802`)
> - the game, its version, and what you were doing when it failed
>
> A log makes the difference between a fixable report and a guess. For network problems, set the
> log filter to `*:Info Service:Debug Service.SSL:Debug WebService:Debug` before reproducing.

---

## Status

Verified working: All officially supported titles.

| | |
| --- | --- |
| Account sign-in | Browser-based, OAuth loopback + PKCE — the emulator never sees your password |
| Hostname redirection | Nintendo online hostnames resolve to the configured Nextendo servers |
| TLS | Handshake with recovered SNI, ALPN pinned to HTTP/1.1 |
| Auth + secure server | Kerberos ticket, matchmaking, session entry |
| NAT check | Both vantage points, sub-second |
| Peer-to-peer | Hole-punching, races completed |
| Play-time sync | Pushed to your Nextendo profile on game exit |
| Presence | Published on sign-in and game start/stop |
| Profile name | Local Switch profile renamed to your account nickname |

## Setup

1. Build as you would upstream Citron (see `docs/`), or use a release build.
2. Open the **NexTendo** menu and click **Enable Network Redirection**. It's off by default; the
   server addresses are already filled in, so there's nothing to type.
3. Still in the **NexTendo** menu, click **Sign In** and complete sign-in in your browser.
4. Launch a supported game and enter its online mode.

`NEXTENDO_SERVER_IP`, `NEXTENDO_NAT_IP` and `NEXTENDO_API` are honoured as environment overrides
if you need to point at something other than the default servers; the API override only accepts
loopback or HTTPS on the Nextendo domain, because those requests carry your account token.

**Friends, requests, and recently played** live under **NexTendo → Open Account Page** — add by
friend code, accept or decline requests, see who's online.

> [!CAUTION]
> Your Network ID (PID) is effectively a credential on this network: the service accepts a bare PID
> as an identity. This fork deliberately never displays or logs it. Don't paste it anywhere, and
> don't ship `nextendo_account.txt` — it holds your session token — inside a build or archive.

## Credits and how this was built

This is [Citron](https://git.citron-emu.org/citron/emu), itself derived from
[yuzu](https://github.com/yuzu-emu/yuzu). All emulation — CPU, GPU, audio, input, filesystem — is
theirs. This fork's changes are confined to the networking and account layers plus the surrounding
UI.

The Nextendo Network client behaviour was worked out by **studying the reference implementation**,
[Ryujinx-Nextendo](https://github.com/NextendoNetwork/Ryujinx-Nextendo), together with the
[published server sources](https://github.com/NextendoNetwork) — which document the protocol, the
endpoints, and the reasons behind a number of non-obvious decisions far better than black-box
guessing ever would. Credit where it is due: several fixes here exist because their comments
explained *why* something was necessary.

**No code from that project is copied into this one.** It could not be: it is licensed under
PolyForm Shield 1.0.0, which is incompatible with Citron's GPL. Everything here is an independent
C++ implementation written against Citron's own IPC, socket, TLS and configuration layers, which
differ substantially from Ryujinx's. Where the two diverge, it is deliberate:

Also referenced: [Kinnay's NintendoClients](https://github.com/kinnay/NintendoClients) for NEX and
error-code documentation, and [switchbrew](https://switchbrew.org) for service definitions.

## Security

- **Sign-in never touches your password.** It's browser-based OAuth loopback + PKCE — the
  emulator only ever sees a short-lived session token, never your credentials.
- **Your Network ID (PID) is never displayed or logged.** It functions as a bearer credential on
  this network, so this fork deliberately keeps it out of the UI and out of `citron_log.txt`.
- **Peer IP addresses are redacted in logs.** Connection logs (socket bind/connect/send/receive,
  and room join/leave/kick/ban events) mask the address before it's written, so a log file pasted
  into a bug report or Discord doesn't hand out another player's IP.
- **Redirection is off by default.** It only activates once you explicitly enable it from the
  NexTendo menu — an unconfigured toggle behaves like stock Citron.
- **The API override is restricted to loopback or HTTPS on the Nextendo domain**, since that
  request carries your account token.
- Your session token lives in `nextendo_account.txt` — don't share it or ship it inside a build or
  archive.

## Legal

Licensed **GPL-3.0-or-later**, as required by Citron. See [LICENSE](LICENSE).

This project ships no Nintendo code, keys, firmware or games, and is not affiliated with, endorsed
by, or associated with Nintendo. You must supply your own legally dumped games and system files,
exactly as with upstream Citron. "Nintendo Switch" and all game titles are trademarks of their
respective owners.

Nextendo Network is a community-run service, independent of this fork and of Nintendo.

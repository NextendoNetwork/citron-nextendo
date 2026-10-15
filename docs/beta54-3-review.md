# Beta54.3 implementation review

This draft submits the implementation shipped to Soul for review by the Citron
maintainers. It is broader than the former documentation-only Classics PR15.
Do not treat this branch as approval to merge or deploy.

## Source and artifact boundary

- Historical client base: `38e110fa46c1b6c5822f5b15db95852bbfd8b278`.
- Imported source commit: `24801b787` (191 changed/added text files).
- Source input: `Citron-build54.3-source.zip`, shipped with the beta54.3 package.
- Windows executable SHA-256:
  `ed6d0c357c6588adb115012dcfd992dd5c9960f3f9adde7eb0fa97b18f3d5ef1`.
- Final portable beta54.3 ZIP SHA-256:
  `1d90990c28118c071c2397e02eb49603387a621625e30b99965abd54a6faaef9`.
- Publication-only differences from that source snapshot: normalize added tabs
  and trailing blank lines in the BSD/SSL and handwritten HOME NAT bridge files;
  replace the developer's absolute FetchContent directory with
  `${CMAKE_BUILD_ROOT}/_deps`. No other runtime changes are claimed here.
- This PR adds the review document and six standalone regression checks after
  the snapshot import. It excludes the later, unvalidated build55 lifetime,
  profile-photo and HOME presence-icon experiments.

## Main reconciliation â€” October 15, 2026

The initial review preserved the historical beta exactly. The branch now merges
`82af7ba5e71dd23cc4b2ad491695bf2b55ffb296` from main. All five conflicts were
resolved explicitly: preserve main's Splatoon 3 package validation and preboot
refresh; keep account-cache C/B buffer placement with the beta caller identity;
use main's routed adapter choice; retain both NSO patch includes; keep TLS
certificate size queries safe without an unconditional duplicate write.

The original beta54.3 artifact hashes above describe the original distributed
beta, not this newly combined candidate. Full candidate build and Linux sanitizer
CI results are tracked below and in the PR. No merge approval is implied.

## Suggested review order

| Area | Entry points | Behavior / review focus |
| --- | --- | --- |
| qlaunch process and applet lifecycle | `src/core/hle/service/am/`, `src/core/core.*`, kernel worker/process changes | Guest system applets, HOME foreground/display/input ownership, process winding, reboot and termination ordering. Verify lock order and suspended-game lifetime. |
| HOME software library | `src/core/hle/service/ns/`, NCM/AOC, registered cache, `game_list_worker.cpp` | Distinguish XCI game-card content and NSP digital content; watch configured folders and external update/DLC directories, including subdirectories. New base games were tested; update/DLC refresh needs more acceptance. |
| Internet and connection test | `network_interface.*`, NIFM, SSL/BSD, `ctest_response.h`, settings server | Host adapter information; connection-test field adaptation and real transfer completion. Speeds are measurements, not estimates based on PC specifications. |
| Last NAT in HOME | `last_nat_cache.h`, `home_nat_patch.S/.h`, NSO loader | Persist only completed NAT results with a network fingerprint; invalidate on network change. The handwritten bridge is restricted by HOME title/build/signatures. Other firmware builds need explicit testing. |
| Friends and profile | friend/ACC services, `nextendo_friends.*`, `nextendo_friend_code.h`, `mypage_member_label.h` | Native friend code/history, presence and account-created timestamp. The label says account membership rather than friendship acceptance; the current replacement text is Spanish and needs localization review. |
| Themes, mouse and GPU bounds | boot manager, HID touch resources, shader cache, `device_address_page_table.h` | Correct mouse event interpretation, per-applet touch isolation, cache format/bounds and the 39-bit device address space. These do not certify every homebrew application. |
| Setup and homebrew | `setup_wizard.inc`, `homebrew_setup.inc`, main/input/hotkeys/settings | Keys and firmware import, external content folders, controller/adapter choices, Nextendo browser sign-in, Discord opt-in and HOME shortcuts. No hardware preset page or BCAT/Smash page in the wizard. |
| Homebrew return | `window_system.cpp`, `library_applet_creator.cpp`, `applet.h` | Ctrl+H / Player1 L+R+Minus opens and returns; HOME requests return on the AM observer. Terminate the marked homebrew applet and prune it before normal parent operation. Plus retains its own application behavior. Finish writes before leaving a tool. |
| BCAT | `bcat_cache_tools.h`, BCAT services and Nextendo controller | Game-specific cache routing and staged replacement that retains the previous cache on failure. Online gameplay acceptance is separate. |
| Additional beta diagnostics | audio/UNO opt-ins and `uch_account_transport.h`, socket/TLS hooks | Older opt-in lab transport/diagnostic work included in the beta source. Not proof of a production UCH deployment. Review these independently from qlaunch fixes. |

## Validation available

- The beta54.3 Windows build linked with MSYS2 clang64 and Ninja, ending in
  `Deployment complete`. This is the historical beta executable, not a fresh
  full build of current main or another OS.
- Six standalone checks were freshly built with Clang 22.1.8 on Windows and
  passed on October 15, 2026: HTTP/JSON field adaptation and malformed responses;
  concurrent domain-object allocation/lookup/close and reentrant destruction;
  synthetic MSBT/Yaz0 bounds and unrelated-text preservation; NAT persistence,
  completion gate, corrupt records and changed networks; native friend-code
  formatting; BCAT staged replacement and failure preservation.
- An initial sandbox run could not load the MSYS2 runtime DLLs (`0xc0000135`).
  The same six executables passed when run with access to the runtime. That
  environment error was not counted as a product test failure or a passing run.
- The HOME/hbmenu shortcut round trip was observed directly: the child process
  terminated successfully, its caller link was cleared, and HOME responded to
  selection afterwards. Soul confirmed physical-controller return after
  correcting an initially mistaken failure report.
- Owner-confirmed earlier checks: NXThemes reboot returns to responsive HOME;
  connection test finishes with download/upload speeds; last NAT remains
  visible; friend code/activity and account membership date appear; a newly
  copied base game appears without restarting; wizard sign-in opens the browser;
  homebrew tools are available without repeating completed setup.
- No personal profiles, credentials, keys, firmware, games, raw logs or homebrew
  binaries are committed. Official homebrew download versions/hashes remain in
  source. The handwritten NAT bridge includes its assembly source.

## Still unresolved / not certified

1. Returning from Splatoon 3 to HOME has produced a freeze/crash. A deferred IPC
   lifetime issue was captured, but its later proposed build55 fix is excluded.
2. Native profile image editing and the missing HOME presence icon still need
   work; they are not repaired by this PR.
3. Themezer/NXThemes mouse and transition coverage needs broader regression
   testing, despite the owner reports after the earlier input fixes.
4. Repeat update/DLC version refresh, removal, slow copies and rapid folder
   changes; the owner's base-game arrival test is not sufficient for all cases.
5. Hardware/OS/firmware breadth, full online gameplay/BCAT compatibility and
   public-network UCH/Classics acceptance remain separate work.
6. Review the OAuth public-client configuration, cross-platform host adapter
   handling, firmware-specific hooks, hardcoded labels, opt-in lab paths and
   packaging policy before production use.

## Reproduce the standalone checks

Install a C++20 compiler, CMake, Threads, fmt 12 headers (the revision in CMakeModules/dependencies.cmake) and nlohmann-json headers. These tests do
not require console firmware or account data, and are independent of the full
emulator build:

```sh
cmake -S tests/beta54_3 -B build/beta54_3-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/beta54_3-tests
ctest --test-dir build/beta54_3-tests --output-on-failure
```

For a non-system JSON install, pass
`-DNLOHMANN_JSON_INCLUDE_DIR=/path/to/json/include`. On Windows, make the compiler
runtime DLLs available in PATH. Test assertions remain enabled in Release.

## Packaging and sign-in

The source PR does not upload executable archives. Clean beta54.3.2 packages
contain the executable, required runtime dependencies and licenses. Source ZIP,
checksums and provenance are separate downloads. No launchers, forms, Markdown,
homebrew cache, profiles or portable `user` directory are included.

Build with `CITRON_NEXTENDO_DESKTOP_CLIENT_ID` (CMake cache/environment) to embed
the public desktop OAuth application identifier. No sidecar is required for
normal sign-in. The developer environment/sidecar override remains compatible.
This identifier is public application metadata; never distribute account files.
Homebrew preparation downloads pinned releases and verifies their hashes onto
the active virtual SD. Existing deliberate portable directories remain supported;
unpack the normal beta into a new directory to use shared Roaming settings.

AI assistance: OpenAI Codex helped implement, investigate, organize and validate
these changes. Manual gameplay/UI results above are explicitly owner-reported
where applicable. Existing upstream work and license notices are retained.

## Expanded checks after maintainer feedback

- Windows Clang 22.1.8: **9/9 passed** after the main reconciliation. The added
  cases exercise actual per-applet touch focus/bounds, GPU device address page
  tables beyond the former 16 GiB limit and viewer presence storage bounds.
- The concurrent domain-object lifecycle test also passed **20 consecutive
  runs** (327,680 append/get/close iterations across eight workers per run).
- The Linux workflow now runs both ordinary and ASan/UBSan configurations and
  repeats the concurrency check in both. Both configurations passed: [Linux CI run 38699834256](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38699834256).
- Existing manual evidence applies to the original beta; new full-emulator
  gameplay and firmware acceptance for the reconciled candidate is pending.

The first Linux build exposed the distro fmt 9 / Citron fmt 12 mismatch before
tests could run. The workflow now pins the same fmt commit as the emulator;
that failed setup run is not counted as a passing sanitizer result.

## Reconciled full build and desktop smoke

The main-reconciled runtime sources at `d9eb39908` were compiled with MSYS2
CLANG64/Clang 22.1.8 on Windows on October 15. Ninja linked the full Citron target
and finished with `Deployment complete`. Candidate executable SHA-256:
`76b8e6dedce3664d3202ead0ec6eb3ec0d73e6fd08972b9729b4ffe40578cfe3`.
This candidate is distinct from the original distributed beta54.3 above.

The existing dependency/build cache was reused through a transactional overlay
of the reviewed sources. Original developer files and executable were backed up
and restored with hashes checked. No build55 runtime experiments were included.
The cached checkout's embedded title still says `38e110fa4-dirty`; use the
candidate hash and source record here instead of that title for provenance.

The first two full-build attempts stopped on environment configuration (missing
MSYS env, then native versus POSIX libusb paths). After setting CLANG64 and
regenerating CMake, the complete build passed. No failed attempt is claimed as
successful validation.

A separate existing test instance, without replacing the stable beta, booted
HOME and completed **three consecutive Ctrl+H -> hbmenu -> Ctrl+H -> HOME**
round trips. JKSV, NXThemes Installer, sphaira and Themezer-NX were visible. Each
homebrew child terminated and was removed with its caller link cleared. HOME
selection responded to the mouse afterwards. The test instance was closed
normally. This does not test a suspended game, physical controller, theme
installation or network gameplay on this candidate.

See [the manual acceptance matrix](beta54-3-acceptance.md) for the remaining
release gates. Keep this PR draft for maintainer review and community testing.

## Clean beta54.3.2 distribution

The Windows full build from `164f171576a21d65270a93ffbc1b5946b35730ba`
passed and completed runtime deployment. Executable SHA-256:
`a7177801e650991e7903ec2a4e1ec99be90e62b1e8ef1d3ad8239791ac941c37`.
Runtime ZIP SHA-256:
`c0cba530c4d8df98c2331c5078696fddb97cf15056fa31ce2528ca3702f38abd`.

The Windows focused suite passed 10/10. A synthetic packaging fixture verified
that user data, homebrew caches, source, launchers and report documents are
excluded while runtime files remain. Final ZIP integrity and content checks
passed. Public OAuth desktop metadata was verified present in the executable;
this verifies compilation, not a fresh account authorization round trip.
Original developer source files and executable were restored and hash checked.
No new desktop/gameplay test was run: the owner deferred their Roaming check.
Tester feedback is requested as a brief English Discord reply, outside the ZIP.

### Final package after current main integration

Main advanced to `e22392f07` while this package was being prepared (signed NPLN
catalog and Windows game relaunch changes). Resolved the two new conflicts by
keeping both Qt includes and applying the existing HOME NAT bridge before the
upstream signed-catalog selector. No old hardcoded NPLN list was retained.

The final beta54.3.2 runtime is built from merge `a18425cb4b852ebb9940d56c1471ecfd024c2c49`.
The complete Windows build passed and deployed; source/executable restoration
was hash checked. Final executable SHA-256:
`e2fa170872fb61672a256b2d5f8ce78e449573388fdc824cb3e293ff76883660`.
Final runtime ZIP SHA-256:
`301b3eac1e3163a532a6b22995ce4b5f396d6fffcaf6fff5d139ac055f884399`.
ZIP integrity, clean contents and compiled public OAuth metadata checks passed.

On this integration, Linux normal and ASan/UBSan focused suites passed
[run 38759707364](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38759707364),
and the upstream signed catalog/loader/runtime checks passed
[run 38759707226](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38759707226).
These results do not change the pending manual acceptance record.

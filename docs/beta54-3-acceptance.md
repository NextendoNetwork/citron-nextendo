# Reconciled beta: manual acceptance record

Back up the tester's active Citron data directory. Record the executable
hash, backend version, firmware, OS/GPU, game/version and controller. Do not attach
keys, account tokens, saves or unredacted logs to a public issue.

The original beta54.3 and the main-reconciled candidate are different binaries.
Historical owner confirmations do not automatically pass the candidate below.

| Test | Procedure / expected result | Reconciled candidate |
| --- | --- | --- |
| Startup | Start normally; completed wizard is not forced again | Pending |
| HOME boot | Start with --home-menu; unlock; navigate using mouse and keyboard | PASS: --home-menu boot and mouse control selection |
| Homebrew round trip | Open Ctrl+H, return Ctrl+H; repeat three times; HOME remains responsive | PASS: three consecutive Ctrl+H cycles, child cleanup verified |
| Controller return | HOME and L+R+Minus from hbmenu; no spontaneous reopen | Pending physical controller |
| Tools available | hbmenu lists the four configured tools; no wizard requirement | PASS: all four tools visible without wizard prompt |
| Settings applet | Open/close System Settings; return to responsive HOME | Pending |
| Profile | Open/close native profile; check friend code/history/member label | Pending account/manual |
| Photo editing | Open native avatar editor and return | Known unresolved; do not mark fixed |
| New content | Copy one complete base game to a configured folder; wait for copy completion | Pending |
| Update/DLC | Add matching external content recursively; reopen Software Information; verify version and removal | Pending |
| Slow copy | Add large update slowly; no parse/crash before completed copy | Pending |
| Themezer/NXThemes | Mouse navigation, preview, download, install and reboot | Pending |
| Connection data | Compare actual adapter type/band/channel/IP/DNS; no inferred bandwidth | Pending host test |
| Connection test | Complete download/upload, return, reopen status; last NAT persists and retests on changed network | Pending live network |
| Suspended game | Splatoon 3 -> HOME for >5 seconds -> resume/switch | Known unresolved freeze/crash |
| BCAT lifecycle | Matching package accepted; incomplete/failing update keeps previous cache; refresh before boot | Pending game/backend acceptance |

A pass for basic HOME/homebrew navigation is not acceptance of a suspended-game
lifetime fix. Keep the PR draft until maintainers approve its remaining scope.

Tested Windows candidate SHA-256:
`76b8e6dedce3664d3202ead0ec6eb3ec0d73e6fd08972b9729b4ffe40578cfe3`.
Normal desktop shutdown also completed. No game was launched in this smoke run.

## Normal Windows distribution

Beta54.3.1 and later omit portable data and use the existing Windows path manager:
without a deliberate `user` folder they share `%APPDATA%\citron`. The additional
setup-startup test covers legacy configuration and launches from two executable
version directories. The Windows focused suite passed 10/10; Linux normal and
ASan/UBSan also passed [run 38702831600](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38702831600).
The owner deferred the normal-startup manual check, which is still pending.
The desktop smoke recorded above applies to the earlier reconciled binary only.

## Profile candidate beta54.3.3

Runtime source `b1d7b6261`; executable SHA-256
`48db169a7701af536bdaae59c10688259c6dec77248282417fef574348163b16`.
Full Windows build/deployment and 11 focused checks passed. Linux normal and
ASan/UBSan passed run 38766376345; catalog/loader/runtime passed run 38766376327.
The owner completed three profile/HOME cycles with their physical controller
and confirmed responsiveness without freezing or closing. Community-tester
hardware confirmation remains separate. This does not pass photo editing,
HOME presence icon rendering or the Splatoon 3 suspension scenario.

## Homebrew extraction candidate beta54.3.4

Runtime source `7bcfea089`; executable SHA-256
`4f92e80d0487074cc45bb6ec97b5c9d6982935ffce7062e081044b3459aa8f68`.
Windows full build/deployment and **12/12** focused checks passed. The real Qt
regression reproduces the invalid Windows default directory name derived from
`citron-neo: The switch fell off`, then validates unique safe directories, writes,
subdirectories and cleanup. Homebrew and mod extraction use the shared safe template.
Linux normal and ASan/UBSan passed
[run 38768319034](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38768319034);
catalog/loader/runtime passed
[run 38768318860](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38768318860).

The actual GUI preparation completed on the active virtual SD. All six pinned
hashes match: hbmenu.nro, hbl.nsp, NXThemesInstaller.nro, themezer-nx.nro,
sphaira.nro and JKSV.nro. Ctrl+H opened native hbmenu, showing JKSV, NXThemes,
Sphaira and Themezer-NX without another wizard requirement. Ctrl+H then returned
to HOME, with native UI still rendering at 60 FPS. The profile fix from
beta54.3.3 remains included; its three controller cycles were owner-confirmed.
The source ZIP and provenance remain separate from the clean runtime archive.

## HOME profile activity candidate beta54.3.5

Runtime source `3a455b4e4`; executable SHA-256
`59b9d1c1b50d998ff7755ef529d886364500c0180e37ec1008da77f10de1b28e`.
Full Windows build/deployment and **13/13** focused tests passed, including
localized NACP strings, icon bounds/signature, HOME-only metadata selection,
viewer fallback, offline handling and retaining actual game presence.
Linux normal and ASan/UBSan passed
[run 38770086795](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38770086795);
catalog/loader/runtime passed
[run 38770086918](https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38770086918).

The profile formerly received application ID zero with only a qlaunch text field.
It now receives the HOME ID and local control metadata for "qLaunch (Switch Menu)"
plus the bundled Nextendo HOME JPEG, without fetching an icon during profile IPC.
The same viewer-only fallback handles friends explicitly using an emulator HOME.
Game-facing friend entries and joinable-session fields remain unchanged. Other
application metadata stays on its existing path.

The owner opened the native profile and confirmed the correct name and icon,
without "Loading failed". This passes HOME presence icon/name acceptance; native
photo editing and the suspended-game scenario are still separate work.
The runtime ZIP integrity and clean-content checks passed. Source and provenance
remain separate artifacts, and original developer source/executable were restored.

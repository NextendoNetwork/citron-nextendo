# Linux x86_64 beta54.3.6 test build

The full Linux build and AppImage packaging passed on GitHub Actions at revision
39d2a4ff2a0f495c547df1a67c8ffb6e4f2ae941 (same runtime changes as the Windows
beta54.3.6, with subsequent documentation and workflow-only changes).

- Build: https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38785076666
- Download: https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38785076666/artifacts/11999900312
- Artifact: Artifacts_x86_64, retained until 2027-01-13 by Actions.
- Includes the standard x86-64 AppImage, zsync sidecar and alternative tar.zst.
- The official nightly release job was skipped for this beta branch.

## Running

Extract the downloaded Actions ZIP. Use the AppImage, or extract the tar.zst and
run its AppRun launcher. For the AppImage:

```sh
chmod +x citron_nightly-39d2a4ff2-linux-x86_64-use-nopgo.AppImage
./citron_nightly-39d2a4ff2-linux-x86_64-use-nopgo.AppImage
```

Normal Linux configuration paths are used; this package contains no configured
user directory, account file, keys, firmware or saved games. On an existing setup,
check that its settings are retained and the first-run wizard is not repeated.

## Verified

- Actions' complete x86_64 build and packaging succeeded.
- The tar.zst was readable; its 1414 entries were checked for unsafe paths and
  personal runtime data filenames.
- The main executable is a 64-bit x86-64 ELF.
- Compiled public desktop OAuth metadata is present (no account credential used).
- AppImage and tar.zst hashes were recorded after download.

AppImage SHA-256:
995f52779623bca355c83c2d1cb1e332187f3533d035c4d56658247d26bed613

Tar.zst SHA-256:
95ca4da446b4fd0c7a70d1a6b0757a30142e0eb016552082ceb204c147f75beb

## Tester acceptance pending

The Linux GUI has not been run on this Windows workstation. Please check startup,
existing settings, browser sign-in, HOME/profile navigation and a game launch.
Report distribution, desktop session (Wayland/X11), GPU/driver and a screenshot
or error if something fails. Do not share accounts, keys, firmware or saves.

The Windows beta limitations still apply: profile banners and News are incomplete;
ambiguous original Ryujinx activity needs the separate sender fix in Ryujinx PR #60.

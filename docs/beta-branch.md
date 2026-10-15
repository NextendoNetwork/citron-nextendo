# Beta branch and Windows shared data

The public beta fork is https://github.com/SoulToxic3119/citron-nextendo and the
branch is `codex/beta54-3-qlaunch-review`. The same commits are mirrored to the
existing upstream PR18 for continuity. It remains draft, not officially merged.

For later changes, fetch NextendoNetwork/main, merge into the beta, resolve the
conflicts, rebuild and repeat automated/manual acceptance before publishing.

```sh
git remote add upstream https://github.com/NextendoNetwork/citron-nextendo.git
git fetch upstream main
git switch codex/beta54-3-qlaunch-review
git merge upstream/main
# resolve, build, test, then push the beta branch to your fork
```

## Windows data and first run

Normal packages do **not** include `user`. The existing path manager therefore
uses `%APPDATA%\citron`, shared between executable versions. Detect a nonempty
qt-config.ini before QtConfig initializes/saves it. Existing configuration,
imported keys, firmware or game folders prevent automatic setup. Older settings
without setupWizardSeen are migrated without reopening the wizard. Tools > Setup
Wizard / --setup-wizard remains available explicitly. The state is saved in the
active data folder, not beside every new executable.

Keep intentional portable installations compatible. Do not delete an old `user`
directory or silently merge it into Roaming. Unpack the normal beta into a fresh
application folder. Homebrew preparation downloads verified tools to the active
virtual SD; the runtime package does not bundle a cache or create portable data.

## Beta artifact CI

Run **Build Windows beta artifact** manually on the beta branch, supplying a
public desktop OAuth application identifier, never an account token. It builds
and uploads a 14-day artifact. It does not publish releases, modify LATEST_VERSION,
post to Discord or deploy servers. It reuses the existing Windows build procedure.
The manual workflow still requires a full CI build acceptance on the fork; its
presence does not establish a successful run.

`tools/package-beta-windows.py` copies only allowed runtime files and licenses.
The runtime ZIP excludes profiles, a portable user folder, homebrew caches,
launchers, report forms, Markdown, source archives and version/provenance files.
Users launch `citron.exe`. Source ZIP, checksums and build provenance are separate
artifacts alongside the runtime ZIP. Supply --binary for a separately built review
executable. Public desktop OAuth metadata is compiled using the CMake cache variable
`CITRON_NEXTENDO_DESKTOP_CLIENT_ID` (initialized from the same environment variable),
so sign-in needs no sidecar file or launcher. Never supply an account token.

Tester communication is a short English Discord message; do not require a form.
Suggested message:

> Citron beta54.3.2 is ready to test. Extract it into a new folder and run citron.exe.
> It uses your existing %APPDATA%\citron settings. Please reply here if something
> fails: tell us what you did, what happened, and your PC/controller details if
> relevant. A screenshot or error message helps. Known issues: returning from
> Splatoon 3 to HOME may freeze; profile photo editing is still being investigated.

The original beta54.3 archive is historical and portable. Beta54.3.1 corrected
normal Roaming startup; beta54.3.2 cleans the runtime distribution. The Roaming
startup check with the user's existing installation is still pending manual test.

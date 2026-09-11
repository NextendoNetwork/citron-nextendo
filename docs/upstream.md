# Upstream tracking

This fork adds a Nextendo Network client to the Citron Android app. Emulation code
stays untouched, with three additive exceptions: `SaveDataFactory` gains
`GetOrCreateTitleSaveDirectory` (cloud-save restores), `NextendoAccount` gains
`UpdateUsername` (keeps the stored account truthful after a rename), and
`NextendoAvatar` exposes `DecodeBase64` (friend avatars). The fork's own code is
confined to the networking, account and surrounding UI layers, plus Android build
plumbing.

## Upstreams

| Repository | Role | Tracked in |
| --- | --- | --- |
| `NextendoNetwork/citron-nextendo` | direct parent (desktop client + core) | `main` |
| `citron-neo/emulator` | Citron Neo (deep upstream) | via parent |

## Branch model

- `main` — upstream state (keep in sync with `NextendoNetwork/citron-nextendo`).
- `android-nextendo` — this fork's work, based on `main`.

Merge upstream into `android-nextendo` with plain `git merge` and commit the
result as a labeled merge commit. Do not rebase `android-nextendo`; rewriting its
history destroys the three-way merge baseline. Keep merges frequent (1-2 weeks).

```
git fetch upstream
git checkout android-nextendo
git merge upstream/main        # resolve conflicts, commit as "merge: upstream main <hash>"
```

## What this fork changes

Each commit is prefixed by area. `git log main..android-nextendo --oneline`
is the authoritative feature list. Reconciliation notes below are for when an
upstream change touches the same lines.

### Files this fork adds (merge-safe by construction)

| File | Purpose |
| --- | --- |
| `src/android/app/src/main/jni/nextendo_jni.cpp` | all Nextendo JNI bindings; own translation unit so upstream `native.cpp` edits never conflict |
| `src/citron/nextendo_byaml.{h,cpp}`, `nextendo_ssbu_mods.{h,cpp}` | Android-only tooling: Splatoon 2 BCAT schedule install, SSBU Skyline mod install |
| `src/android/vcpkg-overlay/arm64-osx.cmake`, `host-toolchain.cmake` | macOS host-triplet fix for vcpkg (see build notes) |
| `src/android/app/src/main/java/.../service/NextendoSignInService.kt` | foreground service keeping the OAuth loopback alive |
| `.../utils/NextendoAccountState.kt`, `NextendoImages.kt`, `NextendoConnectionTest.kt`, `NextendoCloudSaveResult.kt` | cached profile + change events, image decode, NAT probe, result strings |
| `.../fragments/Nextendo*DialogFragment.kt` | profile, friends, cloud saves, play history; share `NextendoDialogFragment` base |
| `.../model/view/SignInStatusSetting.kt`, `.../viewholder/SignInViewHolder.kt`, `list_item_sign_in.xml` | signed-out sign-in row with status pill |
| `.../model/view/ProfileSetting.kt`, `.../viewholder/ProfileViewHolder.kt`, `list_item_profile.xml` | signed-in profile row (avatar + username) |
| `res/xml/network_security_config.xml` | cleartext to loopback only (OAuth callback) |
| `res/drawable/bg_online_pill.xml`, `bg_update_pill.xml`, `bg_status_dot.xml` | game-list badges and the profile status dot |
| `res/values/strings_nextendo.xml`, `colors_nextendo.xml`, `styles_nextendo.xml` | fork strings/colors/styles, kept out of upstream files |

### Shared files this fork edits (conflict-prone)

| File | Change | On conflict |
| --- | --- | --- |
| `src/android/app/build.gradle.kts` | `ENABLE_WEB_SERVICE=1`; relative `VCPKG_OVERLAY_TRIPLETS` arg | keep the arg; re-check the option block |
| `src/android/app/src/main/jni/CMakeLists.txt` | compiles `src/citron/nextendo_save_sync.cpp`, `nextendo_byaml.cpp`, `nextendo_ssbu_mods.cpp`; `CITRON_ENABLE_LIBARCHIVE`, `ENABLE_WEB_SERVICE`, `LibArchive`, `nextendo_jni.cpp` | keep the added lines; if upstream refactors the target, re-apply by intent |
| `src/common/nextendo_account.{h,cpp}` | `EnsureLoaded` only caches successful file reads (Android init-order fix); `UpdateUsername` re-saves the account after a rename | resolve by intent; upstream may fix this differently |
| `src/common/nextendo_avatar.{h,cpp}` | `DecodeBase64` exposed so the Android friends cache can decode avatars | additive; keep |
| `src/core/file_sys/savedata_factory.{h,cpp}` | additive `GetOrCreateTitleSaveDirectory` (recreates the guest's save layout for cloud restores) | keep the method; re-apply it over any upstream rewrite of `GetTitleSaveDirectory` |
| `src/common/settings.h` | `enable_nextendo` default `true` (intentional divergence); `nextendo_friend_notifications` | keep ours unless upstream changes the semantics |
| `src/web_service/nextendo_api.{h,cpp}` | `SetCaCertPathOverride` + CA override; `GetGalleryAvatar`, `WebsiteProfileUrl`; `SetUsername` updates the stored account | additive; re-apply if `ApplyCaCertPath` moves |
| `src/web_service/ssbu_mod_installer.cpp` | applies the Nextendo CA override before the Linux CA candidates (Android has no CA file OpenSSL can read) | keep the override branch first |
| `src/citron/nextendo_save_sync.cpp/h` | compiled into the Android build (desktop file, zero Qt deps); `Pull` creates a missing title save directory; `Pull`/`Push` return typed results; `ExtractZipToDirectory` shared with the BCAT/SSBU modules | keep the creation step; update the CMake reference if upstream moves the file |
| `src/android/.../fragments/EmulationFragment.kt` | before boot: cloud pull, Splatoon 2 BCAT, SSBU mods; account-gate toast; presence/play-time pumps; friend-online notifications | small anchors; re-apply by intent |
| `src/android/.../utils/GameHelper.kt`, `model/GamesViewModel.kt` | the last library scan is cached for screens that need the list without rescanning (cloud save manager) | small anchors; keep |
| `src/android/.../AndroidManifest.xml` | foreground-service permissions + service + network security config | keep the additions |
| `src/android/.../NativeLibrary.kt`, `CitronApplication.kt` | external funs + callbacks + CA export at startup | keep; the Kotlin JNI surface is documented in `nextendo_jni.cpp` |
| `.../features/settings/*` (Settings, presenter, adapter, fragments, `SettingsItem.kt`) | `SECTION_NEXTENDO`, `TYPE_SIGN_IN_STATUS`, `TYPE_PROFILE`, the account-state reload, `onResume` reload, DiffCallback content compare | small anchors; re-apply by intent |
| `.../adapters/GameAdapter.kt`, `ui/GamesFragment.kt`, `fragments/SearchFragment.kt` | pill binding + 30s count polling | keep the additions |

### Desktop files compiled into the Android build

`src/citron/nextendo_save_sync.cpp` is added to `citron-android` because it has
no Qt dependency, with one small modification (`Pull` creates a missing save
directory, `Push`/`Pull` return typed results). Any upstream refactor of that
file (it lives in the Qt target) must be verified against the Android build.
`nextendo_byaml.cpp` and `nextendo_ssbu_mods.cpp` are fork-added and Android-only,
so upstream never touches them.

## Intentional divergences

- `enable_nextendo` defaults to on (upstream desktop defaults it off).
- The macOS vcpkg overlay exists because AGP leaks the NDK clang into `CC/CXX`
  for the host-triplet detection. If upstream ever fixes this properly, delete
  the overlay and the gradle argument.

## Publishing

1. Create the GitHub fork of `NextendoNetwork/citron-nextendo`; push `main` and
   `android-nextendo`.
2. Releases: build with `AUTO_VERSIONED=true` (otherwise `versionCode` stays 1 and
   updaters can't see new builds), tag `vX.Y.Z`, and attach `app-mainline-release.apk`.
3. Build on any host: JDK 17, Android SDK (platform 34, NDK 26.1.10909125,
   cmake 3.22.1). The vcpkg overlay only affects macOS hosts.

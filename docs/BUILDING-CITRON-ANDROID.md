# Building Citron Neo for Android

This document covers building the Android APK from `src/android/`. The result is
a self-contained `app-mainline-release.apk` for arm64 devices with Android 11 or
newer (minSdk 30) and a Vulkan-capable GPU.

- - -
## Requirements

| You need | Version |
| --- | --- |
| JDK | 17 |
| Android SDK platform | 37 |
| Android NDK | 26.1.10909125 |
| CMake | 3.22.1 |
| Host tools | `git`, `cmake`, `nasm`, `glslang-tools`, `pkg-config` |

The SDK platform (`compileSdk`), NDK and CMake versions are pinned in
`app/build.gradle.kts`; other versions may work but are not tested.

- - -
## Quick Start

```bash
# 1. Clone with submodules
git clone --recursive https://github.com/NextendoNetwork/citron-nextendo.git
cd citron-nextendo

# 2. Build the release APK
cd src/android
./gradlew assembleMainlineRelease
```

The APK is written to:

```
src/android/app/build/outputs/apk/mainline/release/app-mainline-release.apk
```

Install it on a device over ADB:

```bash
adb install -r src/android/app/build/outputs/apk/mainline/release/app-mainline-release.apk
```

- - -
## Build details

- The Nextendo Network client is built into the Android target: the account API
  layer (`-DENABLE_WEB_SERVICE=1`) is set in `app/build.gradle.kts`. Leave it on —
  sign-in, the game list badges, friends, presence and cloud saves live behind it.
- FFmpeg, vcpkg and the other third-party dependencies come from the bundled
  submodules; the first build takes considerably longer than the following ones.
- The release variant is signed with the bundled debug key unless
  `ANDROID_KEYSTORE_FILE`, `ANDROID_KEYSTORE_PASS` and `ANDROID_KEY_ALIAS` are set
  in the environment.
- On macOS hosts, `src/android/vcpkg-overlay/` fixes the NDK clang leaking into
  the vcpkg host-triplet detection. It applies automatically and is inert on
  Linux and Windows hosts.

- - -
## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `SDK location not found` | Set `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to your SDK path |
| Missing NDK or CMake during configuration | `sdkmanager "ndk;26.1.10909125" "cmake;3.22.1"` |
| `INSTALL_FAILED_UPDATE_INCOMPATIBLE` when installing | Remove the previous build first: `adb uninstall org.citron.citron_emu` |

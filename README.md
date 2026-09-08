# citron-nextendo-android

Android build of the Nextendo client on Citron Neo (yuzu lineage).

Nextendo was playable on desktop and on the Switch; Android emulation had no
client. This fork fills that gap.

## Features

- Nextendo Network sign-in (browser OAuth, PKCE — the app never sees your password)
- Online hostname redirection to the Nextendo servers (no hosts-file edits)
- Live player counts and version badges on the game list
- Presence, play-time history and cloud save sync

> **AI-assisted development.** Built for my own use on an Android gaming
> handheld, with planning, implementation and testing assisted by agentic AI
> models. Every change was reviewed by me, a software engineer, empirically
> tested on the device, and additionally checked by a second AI model as a judge;
> standard coding and testing practices were applied throughout.
>
> The source is fully open here — inspect, fork, or send pull requests.
>
> **Testing scope.** Verified only on an AYN Thor Max running Android 13, with
> Mario Kart 8 Deluxe online. No other device, Android version, or game has been
> tested — please report issues; I'm open to feedback.

## Install

Download the latest APK from [Releases](../../releases) and install it, then
open Settings -> Nextendo Network and sign in. Launch a supported title and go
online.

Optionally, use [Obtanium](https://github.com/ImranR98/Obtanium) pointed at this
repository's releases for automatic updates.

## Requirements

- Android 13 or newer, arm64
- Vulkan GPU driver (Adreno 740-class or better)
- 6 GB+ RAM
- A Nextendo Network account (create it at https://nextendo.network)

## Build

JDK 17, Android SDK (platform 34, NDK 26.1.10909125, cmake 3.22.1), then:

    cd src/android
    ./gradlew assembleMainlineRelease

APK: `src/android/app/build/outputs/apk/mainline/release/app-mainline-release.apk`

The vcpkg overlay under `src/android/vcpkg-overlay/` is a macOS host-only fix and
applies automatically; other hosts are unaffected.

## Supported titles

Online play is version-pinned by the server. One version per title:

| Title | Version |
| --- | --- |
| Mario Kart 8 Deluxe | 4.0.0 |
| Splatoon 2 (EU/US/JP) | 5.5.2 |
| Splatoon 3 | 11.3.0 |
| Super Smash Bros. Ultimate | 13.0.5 |
| Super Mario Maker 2 | 3.0.3 |
| Animal Crossing: New Horizons | 3.0.3 |
| Mario Tennis Aces | 3.1.1 |
| Luigi's Mansion 3 | 1.4.0 |
| ARMS | 5.5.1 |

Games with the wrong version show a "Requires X" badge on the game list.

## Security

- Sign-in is browser-based OAuth with PKCE; the app only receives a session token.
- The Network ID is a bearer credential on this network; keep the token private.

## Upstream

- Parent: [NextendoNetwork/citron-nextendo](https://github.com/NextendoNetwork/citron-nextendo)
- Citron Neo: [citron-neo/emulator](https://github.com/citron-neo/emulator)
- Lineage: yuzu -> Citron -> Citron Neo -> citron-nextendo -> this fork

See `docs/upstream.md` for what this fork changes and how to merge upstream.

## License

GPL-3.0-or-later (inherited). This project ships no Nintendo code, keys, firmware
or games. Nextendo Network is a community-run service, independent of this fork
and of Nintendo.
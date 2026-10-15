# Nextendo 1.9.3: native profile banners and presence review

## Reference and scope

Reviewed public v1.9.3 sources corresponding to the supplied Windows ZIP.
Its SHA-256 matches the official release asset:
`c5a43b9aa0159ddd72fc1b4ea39d6ea458ebbbd23d93cbdc35f3f344200e23f8`.
The ZIP executable was not launched; personal accounts were not accessed.
This is a source review, not a visual acceptance test of Ryujinx.

## Banners

NextendoFriendPage patches the 22.5.0 MyPage SARC, BFLYT layouts and BNTX
textures: separate friend (1148 x 224) and own (1152 x 224) RGBA banners,
space in the page, circular profile masks and decoration panes.
NextendoFriendProfile fetches the banner URL and crop, paints badges, and
supplies pixels through Ryujinx's GPU texture update path. It bounds its URL
cache to 24 images, downloads to 6 seconds and a byte-size limit. Animated
banners are limited to 60 frames and a 40 ms minimum frame duration.

The disappearing-banner fix retries static uploads until the texture exists
(up to 20 seconds), then confirms three successful uploads. Generation checks
stop work when the selected friend/device changes. Persistent uploads prevent
the applet's memory from restoring the blank texture. These worker waits must
not become blocking IPC calls when implementing a Citron equivalent.

Citron still needs equivalent layout patches, background image/animation cache,
process-scoped texture updates, and cancellation. No banner port or visual
acceptance is claimed by this patch.

## Missing Ryujinx activity identity

NextendoFriends.DoPublish clears app_id and app_detail for every system program,
including HOME, and its request contains no emulator platform name. A receiver
cannot infer Ryujinx or HOME from generic online Emulator activity with empty
app_id/detail. Do not infer the client from the friend's nickname or call every
idle emulator qLaunch.

A companion patch is included at docs/patches/ryujinx-1.9.3-home-presence.patch.
It preserves HOME's actual ID and online status, with an explicit Ryujinx display
label in the existing opaque presence field, which older servers already relay.
Other system programs retain their prior behavior. The patch is prepared for
review and is now submitted as draft Ryujinx PR #60. It is not applied to the
friend's running Ryujinx. Its actual
DoPublish method was compiled with controlled boundaries: HOME, ordinary game
and other system-program cases passed with a fake HTTP transport. A full
Ryujinx runtime has not been built or tested with the patch.

## Citron corrections

- Explicit HOME presence describes qLaunch and its advertised platform.
- HOME NACP uses neutral Nextendo publisher metadata, not Citron for every client.
- Actual history/presence entries missing metadata receive a neutral Nextendo
  placeholder icon. An absent name shows Application (<title ID>), never a
  guessed title. Installed metadata and actual remote names/icons take priority.
- Incomplete snapshots are combined for the same title so they cannot hide a
  name or cover already present in another cached snapshot.
- External control metadata expires after five seconds so placeholders cannot
  pin stale data for the process lifetime. NS IPC makes no network requests.

The unknown program in the screenshot has not been identified. Kazu's running
Ryujinx has not been updated and may still publish the ambiguous activity.

## Verification

14/14 focused Windows regressions passed, including platform descriptions,
unknown idle presence, UTF-8 boundaries, missing metadata and real icon preference.
Full runtime compilation and native acceptance are tracked separately.

## Sources

- https://github.com/NextendoNetwork/Ryujinx-Nextendo/releases/tag/v1.9.3
- https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/v1.9.3/src/Ryujinx.HLE/HOS/NextendoFriendPage.cs
- https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/v1.9.3/src/Ryujinx.HLE/HOS/NextendoFriendProfile.cs
- https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/v1.9.3/src/Ryujinx.Common/Configuration/NextendoFriends.cs

## beta54.3.6 runtime evidence

Runtime revision: fdac8f79392686961769e426e5168baaf50b8591.
The full Windows build and runtime deployment passed. Original developer source
and executable were restored; the stable executable hash remains unchanged.
The clean runtime contains no personal data, launcher scripts or Markdown files.

Executable SHA-256: 597fd1e31be7a2c61e05e07de0fb2348ff3e6c854e9e01ccc7a9e5263916e128.
ZIP SHA-256: 88d101284e3c2d76c62ad230fd81d39502c2b5b8f1de43a9971e3118a332d6ed.

Linux normal and ASan/UBSan checks passed:
https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38775171095
Signed catalog checks passed:
https://github.com/NextendoNetwork/citron-nextendo/actions/runs/38775171099

Native visual acceptance of the new fallback remains pending. No existing app
instance was closed or replaced for this review. Banner rendering remains pending.

## Follow-up: live ambiguous presence confirmed

After the owner reproduced the still-empty activity card in beta54.3.6, a
read-only targeted query to the already authorized friends API confirmed generic
platform `emulator`, empty app_id/jeu/app_detail, and no client identity field.
Only schema and display-presence fields were inspected; no account credentials
or opaque presence data were printed or persisted. The request did not change
any account, friendship or presence.

The required sender fix is submitted for review:
https://github.com/NextendoNetwork/Ryujinx-Nextendo/pull/60
Head: 3aa1141db6387bdfa22c8fae74fde6c93a03c251 (draft).
The reproducible fake-HTTP publisher test is included in that PR.

Citron beta54.3.6 cannot identify a specific client or HOME from those empty
fields. Updating only the receiving Citron does not update the friend's sender.
A Ryujinx build containing the sender fix and live native acceptance are still
needed before claiming the reproduced card is resolved.

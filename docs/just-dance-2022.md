# Just Dance 2022 on Nextendo

Title `0100EA6014BB8000`. The five exact JD service hosts use the configured Nextendo endpoint; other Ubisoft titles do not gain
new DNS rules. The shared Ubisoft frontend routes JD by its application ID while keeping UNO's existing backend.

Two exact main-module builds receive built-in public trust patches. All hook/padding guards are checked before any write; unknown
builds/modules, nonzero conflicting padding and modified hooks are rejected without partial writes. Applying the reviewed bytes twice
is safe. The native X509 verifier remains in place, with only the deployed public CA/frontend anchors and three exact SDK pins added.
No private key, account, supplied game executable, entitlement override or unlock patch is included.

This also carries the tested Windows UDP shutdown correction and NVDEC VP9 segmentation-header reconstruction required by the lab
runtime. The segmentation reader handles tree/prediction probabilities, signed feature data and absolute/delta state.

Evidence: maintainer-confirmed Switch–Citron shared players/scores against the deployed backend; domestic Citron–Citron and
Citron–Ryujinx voting/shared avatars/scores. These observations used the earlier dedicated build carrying these fixes. The current
upstream integration must pass CI and receive its own release smoke test before being advertised to all users.

Tests include exact-build guard conflicts/atomic rejection/idempotence and 48 segmentation bitstream cases. Matching reproducible
ARM64 trust sources, public anchors and IPS hashes are included in the companion Prelude PR under tools/jd22.
Original installed-song goals remain intact. Unlimited content, phone scoring, all optional WDF modes and larger public rooms are
not covered by this acceptance. Old laboratory JD TLS mods should be removed before using the built-in deployed patch.

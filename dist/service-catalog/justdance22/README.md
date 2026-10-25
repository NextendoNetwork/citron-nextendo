# Just Dance 2022 online indicators

Citron already renders the Nextendo online badge, card count and details count for catalog titles. Register the tested JD2022 display version in the fallback table and merge game-catalog-addition.json into the existing signed game catalog. The signed catalog remains authoritative: publishing this server addition is required while the current signed document omits JD2022.

Merge count-source.public.json into the catalog publisher's count sources. The existing aggregate parser accepts only online=true and a nonnegative integer connected count, and drops the entry when the probe fails. The endpoint exposes fresh verified WDF member counts, not account IDs or menu-only sessions. No per-client extra request, authentication bypass or invented player count is added.

Keep all existing catalog entries and increase its revision. The existing publisher signs on the server with its existing key; no keys belong in this PR. Existing JD public trust patches were merged separately in PR43. This PR does not alter those patches or player unlock requirements. Maintainers should check the badge and count on a fresh and warm client after publication.

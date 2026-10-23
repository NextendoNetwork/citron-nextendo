# Nintendo Classics signed-catalog publication inputs

These are **unsigned maintainer inputs**, not a runtime cache or trust override.
The loader continues to require the existing signed catalog and instruction guards.

| Application | Version | Main build ID |
| --- | --- | --- |
| NES | 9.1.0 | A614FFE01A6A3CC1718C2B39E232A10ACDB2999F |
| SNES | 6.0.0 | FE4A4FE0C808B28795AA8D42B171F9BF6093F32C |
| Game Boy | 4.1.0 | AEDF491084F433567EA142D0F77626956798A826 |
| Game Boy Advance | 3.3.0 | 06980CD0757C5020C6EF73E937C5C16595964612 |
| Game Boy Advance | 3.4.0 | C4744859A0EA4C05A67629274CD930ABC1B97DA2 |

The IPS32 records reproduce the three TLS compatibility instruction edits in the
Prelude 4.0.8 patches used for the corresponding Classics builds. They contain no
game image, account information, certificates or private signing material.

## Maintainer rollout

1. Run `python test-files/npln_classics_definitions_test.py`.
2. Verify every original instruction guard against the matching original loader
   image before signing. All four sets of preimages were checked locally: GBA and SNES in earlier
   static analysis, and NES and Game Boy against their original exact-build images
   before this revision. The structural test is not a substitute for checking
   the original images when updating these definitions.
3. Merge these patches with existing catalog entries and these title-index
   additions with existing titles. Preserve other games and withdrawal records.
4. Use the existing publisher and `npln-patches-key-1` signing workflow, a fresh
   monotonic revision and correct ETags/validity dates. Publish the per-build
   manifests before the updated index. No signing key belongs in this repository.
5. On a fresh Citron cache, verify signed discovery, download, matching guards
   and successful application for each exact build. Test offline cached startup,
   disabled entries, wrong builds and mismatched instructions as well.

Merging this client PR alone does **not** publish signed server manifests. A new
Citron binary recognizes these four titles even with the older index and can
query their per-build manifests; it still applies no patch until authenticated
data is available. A signed withdrawal remains authoritative.

Genesis is deliberately absent from these publication inputs: Switch/Ryujinx hosting and gameplay passed public testing, but Citron
suspend/resume remains unresolved and is outside this client rollout. GBA four-device joining
and gameplay have been exercised between Switch/Citron, but lag and intermittent
rejoin errors remain under investigation. Do not advertise all Classics as fully
stable on the basis of patch installation.

GBA 3.4.0 was inspected from the owner's original update. All three exact preimages and unique surrounding contexts match the corresponding 3.3.0 sites, shifted by 0x1AC0. NSO section hashes and simulated IPS32 application passed. Retain the 3.3.0 definition. Real 3.4.0 room/gameplay testing and signed production publication remain pending.

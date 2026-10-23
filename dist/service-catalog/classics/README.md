# Nintendo Classics signed-catalog publication inputs

These are **unsigned maintainer inputs**, not a runtime cache or trust override.
The loader continues to require the existing signed catalog and instruction guards.

| Application | Version | Main build ID |
| --- | --- | --- |
| NES | 9.1.0 | A614FFE01A6A3CC1718C2B39E232A10ACDB2999F |
| SNES | 6.0.0 | FE4A4FE0C808B28795AA8D42B171F9BF6093F32C |
| Game Boy | 4.1.0 | AEDF491084F433567EA142D0F77626956798A826 |
| Game Boy Advance | 3.3.0 | 06980CD0757C5020C6EF73E937C5C16595964612 |

The IPS32 records reproduce the three TLS compatibility instruction edits in the
Prelude 4.0.8 patches used for the corresponding Classics builds. They contain no
game image, account information, certificates or private signing material.

## Maintainer rollout

1. Run `python test-files/npln_classics_definitions_test.py`.
2. Verify every original instruction guard against the matching original loader
   image before signing. GBA and SNES preimages were checked in earlier static
   analysis; NES and Game Boy preimage verification remains a publication gate.
   The structural test is not a substitute for checking the original images.
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

Genesis is deliberately absent from these publication inputs: its public rollout
and new client acceptance are being prepared separately. GBA four-device joining
and gameplay have been exercised between Switch/Citron, but lag and intermittent
rejoin errors remain under investigation. Do not advertise all Classics as fully
stable on the basis of patch installation.

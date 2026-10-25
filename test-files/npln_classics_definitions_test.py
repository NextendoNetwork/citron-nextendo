"""Validate publishable metadata/IPS consistency, not private game preimages."""
import base64
import hashlib
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1] / "dist/service-catalog/classics"
patches = json.loads((root / "patches.unsigned.json").read_text())
titles = json.loads((root / "title-index-additions.json").read_text())
expected = {
    "0100d870045b6000": "A614FFE01A6A3CC1718C2B39E232A10ACDB2999F",
    "01008d300c50c000": "FE4A4FE0C808B28795AA8D42B171F9BF6093F32C",
    "0100c62011050000": "AEDF491084F433567EA142D0F77626956798A826",
    "010012f017576000": "06980CD0757C5020C6EF73E937C5C16595964612",
    "0100b3c014bda000": "4BF9EF5A0A2BA69427F1F0D6E5653EFD4238754A",
}
assert len(patches) == 6 and len(titles) == len(expected)
assert {t["title_id"] for t in titles if t["enabled"]} == set(expected)
assert {p["title_id"] for p in patches} == set(expected)
expected_builds = {(title, build.ljust(64, "0")) for title, build in expected.items()}
expected_builds.add(("010012f017576000", "C4744859A0EA4C05A67629274CD930ABC1B97DA2".ljust(64, "0")))
assert {(p["title_id"], p["build_id"]) for p in patches} == expected_builds
for patch in patches:
    assert (patch["title_id"], patch["build_id"]) in expected_builds
    assert patch["module"] == "main" and patch["enabled"]
    assert patch["offset_basis"] == "nso_header_included"
    assert patch["required_capabilities"] == ["npln.ips32.v1"]
    raw = base64.b64decode(patch["ips32_base64"], validate=True)
    assert hashlib.sha256(raw).hexdigest() == patch["sha256"]
    assert raw.startswith(b"IPS32") and raw.endswith(b"EEOF")
    records, offset = {}, 5
    while offset < len(raw) - 4:
        address, size = struct.unpack_from(">IH", raw, offset)
        assert size == 4 and address not in records
        offset += 6
        assert offset + size <= len(raw) - 4
        records[address] = raw[offset:offset + size]
        offset += size
    assert offset == len(raw) - 4 and len(records) == 3
    assert len(patch["guards"]) == len(records)
    for guard in patch["guards"]:
        original = bytes.fromhex(guard["original_hex"])
        replacement = bytes.fromhex(guard["patched_hex"])
        assert len(original) == len(replacement) == 4 and original != replacement
        assert records.pop(guard["offset"]) == replacement
    assert not records
print("PASS: six exact Classics builds across five titles, title index, IPS32 hashes and guards agree; original-image verification still required before signing")

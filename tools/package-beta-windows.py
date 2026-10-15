#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package a Windows beta without a portable user directory or account data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-revision", default="HEAD")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", args.version):
        parser.error("Invalid version label")
    repo = Path(__file__).resolve().parents[1]
    runtime = args.runtime.resolve()
    if not (runtime / "citron.exe").is_file():
        runtime = runtime / "bin"
    if not (runtime / "citron.exe").is_file():
        parser.error("Runtime must contain citron.exe, directly or under bin")
    app = args.output.resolve() / ("Citron Beta" + args.version)
    if app.exists():
        parser.error("Output application directory already exists; preserve it or choose another output")
    app.mkdir(parents=True)
    # Allow runtime files explicitly; never copy profiles/configuration from the input.
    for file in runtime.iterdir():
        if file.is_file() and (file.suffix.lower() == ".dll" or file.name in
                               {"citron.exe", "citron-updater.exe", "qt.conf", "LICENSE"}):
            shutil.copy2(file, app / file.name)
    for directory in ("platforms", "imageformats", "iconengines", "styles", "tls",
                      "translations", "networkinformation", "multimedia", "LICENSES"):
        if (runtime / directory).is_dir():
            shutil.copytree(runtime / directory, app / directory)
    if args.binary:
        shutil.copy2(args.binary, app / "citron.exe")
    shutil.copy2(repo / "LICENSE", app / "LICENSE")
    if not (app / "LICENSES").exists():
        shutil.copytree(repo / "LICENSES", app / "LICENSES")
    revision = subprocess.check_output(["git", "rev-parse", "--verify", args.source_revision + "^{commit}"], cwd=repo, text=True).strip()
    # Keep source/provenance separate from the runtime users extract and launch.
    source_archive = args.output.resolve() / ("Citron-Beta" + args.version + "-source.zip")
    subprocess.run(["git", "archive", "--format=zip", revision, "-o",
                    str(source_archive)], cwd=repo, check=True)
    assert not (app / "user").exists()
    hashes = {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in app.rglob("*") if p.is_file()}
    provenance = {"version": args.version, "revision": revision, "files": hashes}
    (args.output.resolve() / "build-provenance.json").write_text(
        json.dumps(provenance, indent=2), encoding="utf-8")
    archive = args.output.resolve() / ("Citron-Beta" + args.version + "-Windows-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as package:
        for file in app.rglob("*"):
            if file.is_file():
                package.write(file, file.relative_to(args.output.resolve()))
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
        assert not any("/user/" in name for name in package.namelist())
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + ".sha256").write_text(sha + "  " + archive.name + "\n", encoding="ascii")
    print(json.dumps({"archive": str(archive), "sha256": sha, "revision": revision}))


if __name__ == "__main__":
    main()

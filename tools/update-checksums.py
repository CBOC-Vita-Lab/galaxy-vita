#!/usr/bin/env python3
"""Refresh the release's checksums in one go. Run from anywhere, after Galaxy-Vita.vpk is in the repository root:

    python3 tools/update-checksums.py [--date YYYY-MM-DD]

1. cboc-release.json and docs/RELEASE.json: the VPK's SHA-256 and the SHA-256 of the eboot.bin inside it
   (left as PENDING-VPK while there is no VPK); docs/BUILD-VERIFICATION.json: the same eboot.bin hash.
2. docs/SOURCE-MANIFEST.json: every source file (everything but the player pages, docs/, store/, licenses/
   and the release files) with its SHA-256.
3. SHA256SUMS: every file of the repository but itself, the VPK included.

Files are those git knows about (tracked, or new and not ignored); outside a git checkout the folder is walked.
Either way .git and generated folders (builds, caches, prepared game data) are skipped.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VPK = "Galaxy-Vita.vpk"
PENDING = "PENDING-VPK"
# Never listed: version control, build output, caches, the player's own game data and HD pack.
SKIP_DIRS = {".git", "__pycache__", ".pytest_cache", "build", "build-vgx", "local", "cooked", "prepared-game",
             "hdtex-build", "node_modules"}
SKIP_SUFFIXES = (".pyc", ".pyo")
NOT_SOURCE = ("docs/", "store/", "licenses/", ".github/")
NOT_SOURCE_FILES = {"SHA256SUMS", "cboc-release.json", VPK, "README.md", "START-HERE.txt", "MANUAL-INSTALL.md",
                    "EASY-INSTALL.md", "MANUAL-PREPARE-GAME.md", "CONTROLS.md", "CHANGELOG.md", "KNOWN-ISSUES.md",
                    "CREDITS.md"}


def skipped(path):
    parts = path.split("/")
    return (any(p in SKIP_DIRS or p.startswith("build-") for p in parts[:-1])
            or path.endswith(SKIP_SUFFIXES) or path == "SHA256SUMS")


def repository_files():
    try:
        out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             cwd=ROOT, check=True, capture_output=True).stdout
        names = {p for p in out.decode().split("\0") if p}
    except (OSError, subprocess.CalledProcessError):
        names = set()
        for directory, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith("build-")]
            for name in files:
                names.add(Path(directory, name).relative_to(ROOT).as_posix())
    if (ROOT / VPK).is_file():
        names.add(VPK)   # listed even where git ignores it
    return sorted(p for p in names if not skipped(p) and (ROOT / p).is_file() and not (ROOT / p).is_symlink())


def sha(path):
    value = hashlib.sha256()
    with (ROOT / path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path, data):
    (ROOT / path).write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def package_hashes():
    vpk = ROOT / VPK
    if not vpk.is_file():
        return None
    with zipfile.ZipFile(vpk) as archive:
        if archive.testzip() is not None:
            raise SystemExit(f"{VPK} is damaged")
        runtime = hashlib.sha256(archive.read("eboot.bin")).hexdigest()
    return {"path": VPK, "sha256": sha(VPK), "runtimeSha256": runtime}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", default=None, help="release date for docs/RELEASE.json (default: today, when the VPK is present)")
    args = parser.parse_args()

    package = package_hashes()
    release = json.loads((ROOT / "cboc-release.json").read_text())
    record = json.loads((ROOT / "docs/RELEASE.json").read_text())
    verification = json.loads((ROOT / "docs/BUILD-VERIFICATION.json").read_text())
    if package:
        release["package"] = dict(package)
        record["package"] = dict(package)
        record["date"] = args.date or datetime.date.today().isoformat()
        verification.setdefault("sha256", {})["runtime"] = package["runtimeSha256"]
    else:
        print(f"{VPK} is not here yet: package hashes stay {PENDING}.", file=sys.stderr)
        for data in (release, record):
            data["package"] = {"path": VPK, "sha256": PENDING, "runtimeSha256": PENDING}
    write_json("cboc-release.json", release)
    write_json("docs/RELEASE.json", record)
    write_json("docs/BUILD-VERIFICATION.json", verification)

    files = repository_files()
    sources = [p for p in files if not p.startswith(NOT_SOURCE) and p not in NOT_SOURCE_FILES]
    manifest = {"release": release["releaseId"],
                "scope": "Every source file of the release (decomp/, platform/, vita/, tests/, tools/, armhf/ and the "
                         "root build files) with its SHA-256",
                "files": [{"path": p, "sha256": sha(p)} for p in sources]}
    write_json("docs/SOURCE-MANIFEST.json", manifest)
    files = repository_files()          # SHA256SUMS covers the files just rewritten
    (ROOT / "SHA256SUMS").write_text("".join(f"{sha(p)}  {p}\n" for p in files))
    print(f"SHA256SUMS: {len(files)} files; SOURCE-MANIFEST.json: {len(sources)} files; "
          f"VPK: {package['sha256'] if package else PENDING}")


if __name__ == "__main__":
    main()

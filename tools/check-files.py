#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CBOC-Vita contributors
"""Check an extracted install bundle. No network access or file changes."""

import argparse
import hashlib
from pathlib import Path


def check(folder):
    manifest = folder / "SHA256SUMS"
    if manifest.is_symlink():
        return 0, ["Link not allowed: SHA256SUMS"]
    if not manifest.is_file():
        return 0, ["SHA256SUMS must be a regular file. Extract the whole install ZIP first."]
    try:
        lines = manifest.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        return 0, ["Cannot read SHA256SUMS. Extract the whole install ZIP first."]

    problems = []
    entries = []
    names = set()
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        digest, separator, name = line.partition("  ")
        if not separator or len(digest) != 64 or any(c not in "0123456789abcdefABCDEF" for c in digest):
            problems.append("Invalid checksum on line {}.".format(number))
            continue
        parts = name.split("/")
        if any(p in ("", ".", "..") for p in parts) or any(c in name for c in "\\:\0\r\n"):
            problems.append("Unsafe filename on line {}.".format(number))
            continue
        if name.casefold() in names:
            problems.append("Duplicate filename: " + name)
            continue
        names.add(name.casefold())
        entries.append((digest.lower(), name, parts))
    if not entries and not problems:
        problems.append("SHA256SUMS is empty. Get a fresh copy of the install ZIP.")
    if problems:
        return 0, problems

    checked = 0
    for expected, name, parts in entries:
        path = folder
        linked = False
        for part in parts:
            path = path / part
            if path.is_symlink():
                linked = True
                break
        if linked:
            problems.append("Link not allowed: " + name)
            continue
        if not path.is_file():
            problems.append("Missing or not a regular file: " + name)
            continue
        try:
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
        except OSError:
            problems.append("Missing or unreadable: " + name)
            continue
        if digest.hexdigest() != expected:
            problems.append("Changed or damaged: " + name)
        else:
            checked += 1
    return checked, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1],
                        help="extracted install folder (defaults to the folder above tools/)")
    args = parser.parse_args()
    checked, problems = check(args.directory)
    if problems:
        print("CHECK FAILED. Do not install a damaged or incomplete package.")
        for problem in problems:
            print("- " + problem)
        print("No files were changed.")
        return 1
    print("PASS: {} file(s) match SHA256SUMS. No files were changed.".format(checked))
    print("This checks listed download files, not game compatibility or release approval.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

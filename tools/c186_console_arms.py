#!/usr/bin/env python3
"""Check C186 A/B inputs and package a deterministic, offline operator kit.

These arms are for the C186.1 / C186.2 VPKs (docs/console-tests-C186.md). Their BASE sets GXV_LA88_MIPS=1, which on an integ-3.1
build is an A/B arm (owner, 17 Nov: off until the console A/B; docs/RELEASE-3.0-CHECKLIST.md 0b), not the baseline.
"""
import argparse
import hashlib
from pathlib import Path
import stat
import sys
import zipfile


BASE = {"GXV_VSOUT": "1", "GXV_LA88_MIPS": "1"}
CHANGES = {
    "baseline": {},
    "mips-off": {"GXV_LA88_MIPS": "0"},
    "round-off": {"GXV_HRW41_BOX_ROUND": "0"},
    "plant-full": {"GXV_HR_PLANTMOVE": "0"},
    "pose-full": {"GXV_DIAG_FULL_FAR_POSE": "1"},
    "assist-off": {"GXV_HRW": "0"},
}


def preset(arm):
    return dict(BASE, **CHANGES[arm])


def check_env(arm, path):
    # Intentionally stricter than LoadEnvFile: duplicate keys and ignored
    # lines could disguise a different test even when the last value matches.
    with path.open("rb") as stream:
        data = stream.read(4097)
    if len(data) > 4096:
        return False
    lines = data.replace(b"\r\n", b"\n").split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    expected = [f"{k}={v}".encode("ascii") for k, v in preset(arm).items()]
    return sorted(lines) == sorted(expected)


def snapshot(root):
    entries, ids = {}, set()
    if root.is_symlink() or not root.is_dir():
        raise ValueError("a snapshot must be a real directory")
    pending = [root]
    while pending:
        for p in pending.pop().iterdir():
            st = p.lstat()
            key = p.relative_to(root)
            if stat.S_ISDIR(st.st_mode):
                entries[key] = None
                pending.append(p)
            elif stat.S_ISREG(st.st_mode):
                h = hashlib.sha256()
                with p.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        h.update(block)
                entries[key] = (st.st_size, h.digest())
                ids.add((st.st_dev, st.st_ino))
            else:
                raise ValueError("links and special files are not readback evidence")
    if not ids:
        raise ValueError("no files in snapshot; record expected absence manually")
    return entries, ids


def compare(left, right):
    a, b = left.resolve(strict=True), right.resolve(strict=True)
    if a == b or a in b.parents or b in a.parents:
        raise ValueError("use two separate copies, not the same or nested directories")
    first, ids = snapshot(left)
    second, other_ids = snapshot(right)
    if not ids.isdisjoint(other_ids):
        raise ValueError("the copies share files; make independent readbacks")
    return first == second


def kit(output):
    here = Path(__file__).resolve().parent
    docs = here.parent / "docs"
    sources = {"README.md": docs / "console-qa-C186.md",
               "console-tests-C186.md": docs / "console-tests-C186.md",
               "c186_console_arms.py": here / "c186_console_arms.py",
               "c186_console_receipt.py": here / "c186_console_receipt.py"}
    files = {name: path.read_bytes() for name, path in sources.items()}
    for arm in CHANGES:
        lines = "".join(f"{k}={v}\n" for k, v in preset(arm).items())
        files[f"env/{arm}/env.txt"] = lines.encode("ascii")
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_STORED) as archive:
        for name, data in sorted(files.items()):
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    check = sub.add_parser("check", help="check a copied env.txt against one arm")
    check.add_argument("arm", choices=CHANGES)
    check.add_argument("file", type=Path)
    comp = sub.add_parser("compare", help="compare two independent local snapshot directories")
    comp.add_argument("left", type=Path)
    comp.add_argument("right", type=Path)
    build = sub.add_parser("kit", help="from the source checkout, create a new offline operator ZIP")
    build.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "kit":
            kit(args.output)
            return 0
        if args.action == "check":
            ok = check_env(args.arm, args.file)
            print("CONFIG_MATCH (file only)" if ok else "CONFIG_MISMATCH")
        else:
            ok = compare(args.left, args.right)
            print("COPIES_MATCH (local only)" if ok else "COPIES_MISMATCH")
        return 0 if ok else 1
    except (OSError, ValueError) as exc:
        # Files can have private names: do not echo paths in shared receipts.
        msg = str(exc) if isinstance(exc, ValueError) else type(exc).__name__
        print(f"CHECK_INCOMPLETE: {msg}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

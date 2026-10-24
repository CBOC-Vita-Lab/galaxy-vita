#!/usr/bin/env python3
"""Where the PS Vita build keeps each game file on the memory card.

The Vita build (platform/src/dvd/dvd.cpp, "bucket folders") keeps a directory with MORE than 64 files as sub-folders
_bk00, _bk01, ... of about 32 files each, because the card looks a name up by reading the directory from the start: an open()
in ObjectData (1844 files) costs 75-89 ms, in a folder of 32 files a few.  The folder of a file is

    FNV-1a(32 bit) of its lower-case name  modulo  ceil(files in the directory / 32)

where the number of files is counted in the disc's file table (sys/fst.bin), so the layout follows from fst.bin alone.  Four
directories qualify (ObjectData 58 folders, StageData 8, LayoutData 3, AudioRes/Stream 3; 2241 files).

The game rearranges a flat copy itself at its first start (one rename per file, about 5 minutes on the console, the loading
screen shows "Organising the game's files"), so a copy that is put on the card in this layout from the start saves that.
This tool computes the layout and applies it to a cooked tree:

  vita_layout.py <cooked dir>                    one line per file that moves: <path in the cooked tree> TAB <path on the card>
  vita_layout.py <cooked dir> --dirs             one line per bucketed directory: <directory> TAB <number of bucket folders>
  vita_layout.py <cooked dir> --verify [root]    check that every file is where the layout says, in <root> (default: the cooked dir)
  vita_layout.py <cooked dir> --apply            rearrange the cooked tree itself (rename; safe to run again; a flat file replaces an
                                                 older copy in its bucket folder, so it also works after a second cook)
  vita_layout.py <cooked dir> --apply --out DIR  build a laid-out copy of the whole tree in DIR (hard links, or copies where the
                                                 folders are on different disks or --copy is given); <cooked dir> stays flat
  vita_layout.py <cooked dir> --flatten          move the files back out of the bucket folders (the desktop build and the
                                                 emulator's flat copies expect that)

Paths are relative to the cooked dir ("files/ObjectData/Foo.arc" -> "files/ObjectData/_bk07/Foo.arc").  The game still finds a
file that is in the old place and moves it at its next start, and PETARI_BUCKETS=0 makes it use the flat layout, so nothing
breaks if this is not applied.  Keep the constants in step with dvd.cpp (kBucketMinFiles, kBucketFiles, the hash).
"""
import os
import shutil
import struct
import sys

MIN_FILES = 64    # kBucketMinFiles: a directory with MORE files than this is bucketed
PER_BUCKET = 32   # kBucketFiles


def bucket_of(name: bytes, n: int) -> int:
    """FNV-1a of the lower-case (ASCII, C locale) name, modulo the number of bucket folders."""
    h = 2166136261
    for c in name:
        if 65 <= c <= 90:
            c += 32
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h % n


def read_fst(path):
    """The disc's file table as a list of (is_dir, name, a, b): a directory has (parent, next entry after it), a file (offset, size)."""
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack(">I", data[8:12])[0]
    strings = count * 12

    def name(i):
        off = struct.unpack(">I", data[i * 12:i * 12 + 4])[0] & 0x00FFFFFF
        return data[strings + off:data.index(b"\0", strings + off)]

    entries = []
    for i in range(count):
        e = data[i * 12:i * 12 + 12]
        a, b = struct.unpack(">II", e[4:12])
        entries.append((e[0] != 0, name(i) if i else b"", a, b))
    return entries


def layout(fst_path):
    """-> (moves, dirs): moves is [(path in a flat tree, path in the bucketed one)] for every file in a bucketed directory,
    dirs is [(directory, number of bucket folders)]."""
    entries = read_fst(fst_path)
    count = len(entries)
    dir_path = [""] * count
    stack = [0]
    for i in range(1, count):
        while len(stack) > 1 and i >= entries[stack[-1]][3]:
            stack.pop()
        is_dir, nm, _a, _b = entries[i]
        if is_dir:
            parent = dir_path[stack[-1]]
            dir_path[i] = (parent + "/" if parent else "") + nm.decode("latin-1")
            stack.append(i)
    moves, dirs = [], []
    for d in range(count):
        if not entries[d][0]:
            continue
        end = count if d == 0 else entries[d][3]
        kids, i = [], d + 1
        while i < end:
            if entries[i][0]:
                i = entries[i][3]
            else:
                kids.append(i)
                i += 1
        if len(kids) <= MIN_FILES:
            continue
        n = (len(kids) + PER_BUCKET - 1) // PER_BUCKET
        dirs.append((dir_path[d], n))
        prefix = "files/" + (dir_path[d] + "/" if dir_path[d] else "")
        for k in kids:
            nm = entries[k][1]
            moves.append((prefix + nm.decode("latin-1"), prefix + "_bk%02u/" % bucket_of(nm, n) + nm.decode("latin-1")))
    return moves, dirs


def _fst(cooked):
    path = os.path.join(cooked, "sys", "fst.bin")
    if not os.path.isfile(path):
        sys.exit("%s holds no sys/fst.bin: give the folder cook.py wrote" % cooked)
    return path


def verify(cooked, root=None):
    """-> (files in the layout, [paths that are not where the layout says])."""
    moves, _dirs = layout(_fst(cooked))
    root = root or cooked
    return len(moves), [dst for _src, dst in moves if not os.path.exists(os.path.join(root, dst))]


def apply_in_place(cooked):
    """Renames the files of the cooked tree into their bucket folders. -> (moved, already there, missing)."""
    moves, _dirs = layout(_fst(cooked))
    moved = there = missing = 0
    for src, dst in moves:
        s, d = os.path.join(cooked, src), os.path.join(cooked, dst)
        if os.path.exists(s):
            # a flat file is the newest copy (cook.py writes flat, so a second cook into a laid-out tree leaves fresh files here
            # and old ones in the bucket folders): it replaces whatever is in the bucket folder
            os.makedirs(os.path.dirname(d), exist_ok=True)
            os.replace(s, d)
            moved += 1
        elif os.path.exists(d):
            there += 1
        else:
            missing += 1
    return moved, there, missing


def apply_copy(cooked, out, copy=False):
    """Builds the laid-out tree in `out` from the flat one (or one that is already laid out). -> (files, linked, copied)."""
    moves, _dirs = layout(_fst(cooked))
    dest = dict(moves)
    cooked = os.path.abspath(cooked)
    out = os.path.abspath(out)
    if out == cooked or out.startswith(cooked + os.sep):
        sys.exit("--out must be a folder of its own, not inside the cooked folder")
    files = linked = copied = 0
    for dirpath, _dirnames, filenames in os.walk(cooked):
        for fn in filenames:
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, cooked).replace(os.sep, "/")
            rel = dest.get(rel, rel)  # (a file that is already in its bucket folder maps to itself: dest has no such key)
            dst = os.path.join(out, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst):
                os.remove(dst)
            files += 1
            if not copy:
                try:
                    os.link(src, dst)
                    linked += 1
                    continue
                except OSError:
                    pass
            shutil.copy2(src, dst)
            copied += 1
    return files, linked, copied


def flatten(cooked):
    """Moves the files back out of the bucket folders. -> number moved."""
    moves, dirs = layout(_fst(cooked))
    moved = 0
    for src, dst in moves:
        s, d = os.path.join(cooked, src), os.path.join(cooked, dst)
        if os.path.exists(d) and not os.path.exists(s):
            os.replace(d, s)
            moved += 1
    for directory, n in dirs:
        for b in range(n):
            try:
                os.rmdir(os.path.join(cooked, "files", directory, "_bk%02u" % b))
            except OSError:
                pass
    return moved


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    cooked = argv[1]
    rest = argv[2:]
    if "--dirs" in rest:
        for d, n in layout(_fst(cooked))[1]:
            print("files/%s\t%d" % (d, n))
    elif "--verify" in rest:
        i = rest.index("--verify")
        root = rest[i + 1] if i + 1 < len(rest) and not rest[i + 1].startswith("--") else None
        total, bad = verify(cooked, root)
        for p in bad[:20]:
            print("missing:", p)
        print("%d files in bucket folders by the layout, %d not where it says" % (total, len(bad)))
        return 1 if bad else 0
    elif "--apply" in rest:
        if "--out" in rest:
            out = rest[rest.index("--out") + 1]
            files, linked, copied = apply_copy(cooked, out, copy="--copy" in rest)
            print("%d files into %s (%d hard links, %d copies)" % (files, out, linked, copied))
        else:
            moved, there, missing = apply_in_place(cooked)
            print("%d files moved into bucket folders, %d already there, %d not found" % (moved, there, missing))
            return 1 if missing else 0
    elif "--flatten" in rest:
        print("%d files moved back out of the bucket folders" % flatten(cooked))
    else:
        for src, dst in layout(_fst(cooked))[0]:
            print("%s\t%s" % (src, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

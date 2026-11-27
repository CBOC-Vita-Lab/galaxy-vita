#!/usr/bin/env python3
"""A flat view of a cooked game tree that is in the Vita's bucket layout, made of symbolic links (nothing is copied or moved).

The Vita build keeps the big directories (ObjectData, StageData, LayoutData, AudioRes/Stream) as bucket folders _bk00, _bk01, ...
(platform/src/dvd/dvd.cpp, tools/cook/vita_layout.py), and a tree the console build or Vita3K has run on is in that layout.  The
armhf harness reads the flat layout (the bucket code is Vita only), so it cannot boot from such a tree: the first archive in a
bucket folder loads as nothing (JKRMemArchive::mountFixed faults on a null archive).  This writes

    <out>/sys                       -> <cooked>/sys
    <out>/files/<dir>/<name>        -> <cooked>/files/<dir>/<name>  or  <cooked>/files/<dir>/_bkNN/<name>
    (nothing else: the harness reads only sys/ and files/)

so the harness reads the cooked tree in place.  <out> must not exist.

    python3 -I tools/pgo/flat_view.py <cooked dir> <out dir>
"""
import os
import re
import sys

BUCKET = re.compile(r"_bk\d\d$")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cooked, out = (os.path.abspath(p) for p in sys.argv[1:])
    if not os.path.isfile(os.path.join(cooked, "sys", "fst.bin")):
        sys.exit(f"flat_view.py: {cooked} has no sys/fst.bin")
    if os.path.exists(out):
        sys.exit(f"flat_view.py: {out} exists")
    os.makedirs(out)
    os.symlink(os.path.join(cooked, "sys"), os.path.join(out, "sys"))
    links = buckets = 0
    for top, dirs, files in os.walk(os.path.join(cooked, "files")):
        rel = os.path.relpath(top, os.path.join(cooked, "files"))
        parts = [p for p in rel.split(os.sep) if p != "."]
        if parts and BUCKET.match(parts[-1]):   # a bucket folder: its files belong to the directory above it
            parts = parts[:-1]
            buckets += 1
        dest = os.path.join(out, "files", *parts)
        os.makedirs(dest, exist_ok=True)
        dirs.sort()
        for name in sorted(files):
            link = os.path.join(dest, name)
            if os.path.lexists(link):   # (a file in both places: os.walk lists the flat one first, which dvd.cpp also prefers)
                continue
            os.symlink(os.path.join(top, name), link)
            links += 1
    print(f"flat view: {out}: {links} files linked, {buckets} bucket folders flattened")


if __name__ == "__main__":
    main()

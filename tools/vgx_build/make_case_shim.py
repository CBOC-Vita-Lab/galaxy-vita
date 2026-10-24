#!/usr/bin/env python3
"""Make the tree buildable on a case-SENSITIVE filesystem without editing it.

Some #include directives only resolve on case-insensitive filesystems, e.g.
    #include <JSystem/JAudio2/JASDSPInterface.hpp>      (real file: JASDspInterface.hpp)
    #include "Game/Map/FileSelectItem.hpp"              (real file: FIleSelectItem.hpp)
This scans every #include in decomp/, platform/ and vita/, and for each one that does not
resolve exactly but does resolve ignoring case, creates a symlink <shim>/<path-as-written> ->
<real file>.  Put the shim on the include path *last* (CPATH=<shim>), so it can only ever
supply headers that would otherwise be "file not found".  Nothing under the source tree is
modified.   usage: make_case_shim.py <galaxy-root> <shim-dir>
"""
import os
from functools import lru_cache
import re
import sys
from pathlib import Path

root, shim = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
if shim.exists():
    sys.exit("Case shim directory already exists; choose a new build directory")
include_dirs = [root / p for p in (
    "vita/include", "platform/include", "decomp/include", "decomp/libs/JSystem/include",
    "decomp/libs/RVL_SDK/include", "decomp/libs/nw4r/include", "decomp/libs/RVLFaceLib/include",
    "vita/runtime", "platform/src/gx", "platform/src/xr")]
sysroot_includes = [Path(os.environ.get("VITASDK", "/opt/vitasdk")) / "arm-vita-eabi/include", Path("/usr/include")]


@lru_cache(maxsize=None)
def directory_names(path):
    return set(os.listdir(path)) if path.is_dir() else set()


def exact_file(base, relative):
    current = base
    for part in relative.split("/"):
        if part not in directory_names(current):
            return False
        current = current / part
    return current.is_file()

# case-insensitive index of every file below each include dir: lower(rel) -> real rel
index = {}
for d in include_dirs:
    m = {}
    for dirpath, _, files in os.walk(d):
        for f in files:
            rel = os.path.relpath(os.path.join(dirpath, f), d)
            m.setdefault(rel.lower(), rel)
    index[d] = m

pattern = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
exts = {".c", ".cpp", ".h", ".hpp", ".inc", ".hh", ".cc"}
links = {}      # path as written -> absolute real file
unresolved = {}  # path as written -> first includer (not found even ignoring case)
scanned = 0
for top in ("decomp", "platform", "vita"):
    for dirpath, _, files in os.walk(root / top):
        for f in files:
            if os.path.splitext(f)[1] not in exts:
                continue
            path = Path(dirpath) / f
            try:
                text = path.read_text(errors="replace")
            except OSError:
                continue
            scanned += 1
            for kind, inc in pattern.findall(text):
                inc = inc.strip().replace("\\", "/")
                if inc.startswith("./"):
                    inc = inc[2:]
                if Path(inc).is_absolute() or ".." in inc.split("/"):
                    continue
                # exact hit: relative to the includer (quoted form), any -I dir, or a system dir
                if kind == '"' and exact_file(path.parent, inc):
                    continue
                if any(exact_file(d, inc) for d in include_dirs + sysroot_includes):
                    continue
                hit = None
                if kind == '"':
                    # case-insensitive relative to the includer's own directory
                    parent = path.parent
                    cur = parent
                    ok = True
                    for comp in inc.split("/"):
                        match = None
                        if cur.is_dir():
                            for e in os.listdir(cur):
                                if e.lower() == comp.lower():
                                    match = e
                                    break
                        if match is None:
                            ok = False
                            break
                        cur = cur / match
                    if ok and cur.is_file():
                        hit = cur
                if hit is None:
                    for d in include_dirs:
                        real = index[d].get(inc.lower())
                        if real:
                            hit = d / real
                            break
                if hit is not None:
                    links.setdefault(inc, hit)
                else:
                    unresolved.setdefault(inc, str(path.relative_to(root)))

shim.mkdir(parents=True, exist_ok=True)
for inc, real in sorted(links.items()):
    dest = shim / inc
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.symlink_to(real)
print(f"scanned {scanned} files; {len(links)} case-mismatched includes shimmed into {shim}")
for inc, real in sorted(links.items()):
    print(f"  {inc}  ->  {real.relative_to(root)}")
print(f"{len(unresolved)} other includes do not resolve even ignoring case (std/system/SDK headers and sources not in the build; expected)")

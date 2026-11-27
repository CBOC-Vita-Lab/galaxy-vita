#!/usr/bin/env python3
"""Every BP 0xFF marker kind (PORT_GX_MARK_*, port/compat.h) has one meaning: no two names share a value, and the raw
kinds written by GXTev.c's TEV verify pair are the named ones. (Kinds 28/29 were BGLAYER's and TEVCOL=2's at once.)"""
import re, sys, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
txt = (root / "platform/include/port/compat.h").read_text()
kinds = {}
for m in re.finditer(r"^#define\s+(PORT_GX_MARK_\w+)\s+(\d+)\b", txt, re.M):
    kinds.setdefault(int(m.group(2)), []).append(m.group(1))
bad = {k: v for k, v in kinds.items() if len(v) > 1}
ok = True
if len(kinds) < 20:
    print("FAIL: only %d marker kinds parsed (parser stale?)" % len(kinds)); ok = False
for k, v in sorted(bad.items()):
    print("FAIL: marker kind %d used by %s" % (k, ", ".join(v))); ok = False
tev = (root / "decomp/src/RVL_SDK/gx/GXTev.c").read_text()
if re.search(r"0xFFu << 24\)[^;]*\|\s*\d+u\)", tev):
    print("FAIL: GXTev.c writes a marker with a raw numeric kind"); ok = False
print("test_gx_mark_kinds: %s (%d kinds)" % ("PASS" if ok else "FAIL", len(kinds)))
sys.exit(0 if ok else 1)

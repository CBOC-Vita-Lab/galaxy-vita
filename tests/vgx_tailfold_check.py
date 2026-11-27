#!/usr/bin/env python3
"""GXV_TAILFOLD=3 pictures (tools/test_vgx_tailfold_gl.sh): the folded picture against the unfolded one, scenario by scenario, RGB only (the display's alpha is not shown).
   Scenarios that fold (bloom, nohud, fade, hudonly) must differ in 0 pixels and make fewer scenes; the refused ones (nomarker, noputback, depth, dstalpha, copyafter, live, capture) must not fold and
   must not differ; the rung that is not 1:1 (720x404) folds nothing. The fill-rule scenario (ties, EFB of the render size) is reported, and is the only one allowed to differ: its differing
   pixels are counted and must lie on rows of rectangle edges that sit on a pixel centre."""
import sys, os
out = sys.argv[1]
def load(tag, name):
    p = os.path.join(out, tag, name + ".rgba")
    d = open(p, "rb").read()
    t = open(os.path.join(out, tag, name + ".txt")).read().split()
    kv = dict(x.split("=", 1) for x in t if "=" in x)
    return d, {k: v for k, v in kv.items()}
def diff(a, b):
    n = 0
    rows = set()
    for i in range(0, len(a), 4):
        if a[i:i + 3] != b[i:i + 3]:
            n += 1
            rows.add((i // 4) // 960)
    return n, rows
fails = 0
def check(ok, msg):
    global fails
    print(("PASS: " if ok else "FAIL: ") + msg)
    if not ok: fails += 1
FOLD = ["bloom", "nohud", "fade", "hudonly"]
REFUSE = ["nomarker", "noputback", "depth", "dstalpha", "copyafter", "live", "capture", "reader"]
for tag0, tag3, cfg in (("off", "on", "960x544 render, EFB 640x456"), ("off-native", "on-native", "960x544 render, EFB 960x544")):
    for name in FOLD + REFUSE + ["ties"]:
        a, ka = load(tag0, name)
        b, kb = load(tag3, name)
        n, rows = diff(a, b)
        folded = int(kb["folded"])
        sa, sb = int(ka["scenes"]), int(kb["scenes"])
        if name in FOLD:
            check(folded == 1 and n == 0 and sb < sa, f"[{cfg}] {name}: folded, {n} differing pixels, scenes {sa} -> {sb}")
        elif name == "ties":
            print(f"INFO [{cfg}] ties: folded {folded}, {n} differing pixels on {len(rows)} rows, scenes {sa} -> {sb}")
        elif name == "reader":
            check(n == 0, f"[{cfg}] {name}: {n} differing pixels, folded {folded} (a later reader of the capture does not stop the fold), scenes {sa} -> {sb}")
        else:
            check(folded == 0 and n == 0 and sa == sb, f"[{cfg}] {name}: refused (folded {folded}), {n} differing pixels, scenes {sa} -> {sb}")
for name in FOLD + REFUSE + ["ties"]:
    a, ka = load("off720", name)
    b, kb = load("on720", name)
    n, _ = diff(a, b)
    check(int(kb["folded"]) == 0 and n == 0, f"[720x404] {name}: not folded, {n} differing pixels")
# the counters mode 1 does not fold
for name in FOLD:
    a, ka = load("off", name)
    b, kb = load("count", name)
    n, _ = diff(a, b)
    check(int(kb["folded"]) == 0 and n == 0 and ka["scenes"] == kb["scenes"], f"[count only] {name}: nothing folded, same picture and scenes")
sys.exit(1 if fails else 0)

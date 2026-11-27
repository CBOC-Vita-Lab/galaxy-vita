#!/usr/bin/env python3
# Compares the pictures tools/test_vgx_hud_native_gl.sh wrote (tests/vgx_hud_native_gl.cpp) and prints PASS / FAIL lines.
# off720 / on720 / dry720: GXV_HUD_NATIVE 0 / 1 / 2 at the 720x404 rung; off960 / on960: 0 / 1 at 960x544; off848 / on848 at 848x480.
# Also writes side-by-side crops (off | on) to <dir>/crops/*.png and a native-reference comparison.
import os, re, sys
import numpy as np
from PIL import Image

W, H = 960, 544
d = sys.argv[1]
fails = 0


def load(tag, name):
    return np.fromfile(os.path.join(d, tag, name + ".rgba"), dtype=np.uint8).reshape(H, W, 4)


def meta(tag, name):
    t = open(os.path.join(d, tag, name + ".txt")).read()
    f = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", t.split("[")[0])}
    f["calls"] = tuple(int(x) for x in re.search(r"calls=(\d+)/(\d+)/(\d+)", t).groups())
    f["why"] = re.findall(r"\[([^\]]+)\]", t)
    return f


def ok(cond, msg):
    global fails
    print(("PASS: " if cond else "FAIL: ") + msg)
    if not cond:
        fails += 1


def dilate(mask, r):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def diff(a, b):
    return np.any(a != b, axis=2)


names = ["hud3d", "hudonly", "tail3d", "copymid", "nohud", "depthlast", "dstalpha", "twodisp", "hudn", "lines", "refuse", "capture",
         "model", "modelonly", "modelnoclear", "modellast", "modelnonhud", "skipdw"]
# the 2D layer's own pixels: the 960x544 rung's hud-only picture against the same picture without the HUD
grey = np.full((H, W, 4), 90, np.uint8)
grey[:, :, 3] = 255
ref_hud = load("off960", "hudonly")
hudmask = diff(ref_hud, grey) | diff(load("off960", "hud3d"), load("off960", "nohud"))
hudmask_d = dilate(hudmask, 4)
print("hud mask: %d pixels (%.1f%% of the screen), dilated %d" % (hudmask.sum(), 100.0 * hudmask.sum() / (W * H), hudmask_d.sum()))

# 1. dry run changes nothing
for n in names:
    ok(np.array_equal(load("off720", n), load("dry720", n)) and meta("off720", n)["scenes"] == meta("dry720", n)["scenes"], "dry run (=2) leaves %s identical, same scenes" % n)

# 2. the census: which pictures go native
native = {"hud3d", "hudonly", "tail3d", "copymid", "hudn", "lines", "model", "modelonly", "modelnoclear", "modellast", "modelnonhud", "skipdw"}
for n in names:
    m = meta("on720", n)
    ok((m["taken"] == 1) == (n in native), "%s: taken natively = %s (taken=%d, %s)" % (n, n in native, m["taken"], ",".join(m["why"]) or "-"))
    want = (0, 1, 1) if n in native else ((2, 0, 0) if n == "twodisp" else ((1, 1, 0) if n == "refuse" else (1, 0, 0)))
    ok(m["calls"] == want, "%s: hook calls (phase 0/1/2) = %s, expected %s" % (n, m["calls"], want))
ok(meta("on720", "tail3d")["partial"] == 1 and meta("on720", "copymid")["partial"] == 1 and meta("on720", "modelnoclear")["partial"] == 1 and meta("on720", "skipdw")["partial"] == 1, "partial tails counted (3D draw / capture in the middle of the 2D)")
ok(all(meta("on960", n)["taken"] == 0 for n in names), "960x544 rung: nothing taken (the 2D is native already)")

# 3. scene counts equal
for n in names:
    a, b = meta("off720", n)["scenes"], meta("on720", n)["scenes"]
    ok(b <= a and (a - b <= 1) and (a == b or n == "copymid"), "%s: scenes %d (off) vs %d (on)%s" % (n, a, b, " (the 2D after a capture goes into the display's scene: one EFB scene less)" if a != b else ""))

# 4. the fallbacks are exactly today's pictures
for n in ("nohud", "depthlast", "dstalpha", "twodisp", "refuse", "capture"):
    ok(np.array_equal(load("off720", n), load("on720", n)), "%s: falls back, picture identical to off" % n)
for n in names:
    ok(np.array_equal(load("off960", n), load("on960", n)), "%s at 960x544: identical to off" % n)

# 5. the 3D region is today's upscaled 3D, bit for bit
mask_cm = dilate(hudmask | diff(load("off960", "copymid"), load("off960", "hud3d")), 4)  # (copymid also draws a textured 2D rectangle)
def scen_mask(n):  # everything the scenario adds to the plain world, from the 960x544 rung's own pictures
    return dilate(hudmask | diff(load("off960", n), load("off960", "nohud")), 4)
for n in ("hud3d", "tail3d", "copymid", "hudn", "model", "modellast", "modelnonhud", "modelnoclear", "skipdw"):
    a, b = load("off720", n), load("on720", n)
    df = diff(a, b)
    mk = mask_cm if n == "copymid" else (scen_mask(n) if n.startswith("model") or n == "skipdw" else hudmask_d)
    outside = df & ~mk
    ok(outside.sum() == 0, "%s: 3D region identical outside the 2D pixels (+4 px): %d differing pixels outside, %d inside" % (n, outside.sum(), (df & mk).sum()))
    ok((df & mk).sum() > 0, "%s: the 2D region does change (sharper)" % n)

# 6. the 2D region equals a native-resolution 2D render
for n in ("hudonly", "lines", "modelonly"):
    a, b = load("off960", n), load("on720", n)
    df = diff(a, b)
    ok(df.sum() <= 0.002 * W * H, "%s: on (720 rung) vs a native 960x544 render: %d pixels differ (%.3f%% of the screen; edge-tie pixels)" % (n, df.sum(), 100.0 * df.sum() / (W * H)))
    mx = int(np.abs(a.astype(int) - b.astype(int)).max())
    print("  %s: largest channel difference %d, differing pixels %d, of them in the 2D mask %d" % (n, mx, df.sum(), (df & hudmask_d).sum()))
    ok(df.sum() == (df & dilate(diff(load("off960", n), grey), 4)).sum(), "%s: every difference lies in the 2D region" % n)
    c = load("off720", n)
    dc = diff(a, c)
    print("  %s: today's 720 rung vs the native render: %d pixels differ" % (n, dc.sum()))
    ok(dc.sum() > 5 * max(df.sum(), 1), "%s: the 2D layer is much closer to native with the switch on (%d vs %d differing pixels)" % (n, df.sum(), dc.sum()))

# 6b. a host without display depth: depth-tested models stay in the EFB (partial tails, the pictures of round 1)
for n in ("model", "modellast", "modelnonhud"):
    m = meta("nodepth720", n)
    ok(m["partial"] == 1 or m["taken"] == 0, "%s without display depth: tail ends at the depth-tested draw (taken=%d partial=%d)" % (n, m["taken"], m["partial"]))
    df = diff(load("off720", n), load("nodepth720", n))
    ok((df & ~scen_mask(n)).sum() == 0, "%s without display depth: 3D identical outside the 2D pixels" % n)
for n in ("model", "modellast", "modelnonhud"):
    ok(meta("on720", n)["tail"] > meta("nodepth720", n)["tail"], "%s: with display depth the tail is longer (%d vs %d 2D draws)" % (n, meta("on720", n)["tail"], meta("nodepth720", n)["tail"]))
# 7. 848x480 rung, the same 3D identity
for n in ("hud3d", "hudonly"):
    a, b = load("off848", n), load("on848", n)
    df = diff(a, b)
    ok((df & ~hudmask_d).sum() == 0 and meta("on848", n)["taken"] == 1, "%s at 848x480: 3D identical outside the 2D region, taken natively" % n)

# crops: off | on | native reference
os.makedirs(os.path.join(d, "crops"), exist_ok=True)


def crop(img, box, scale):
    x0, y0, x1, y1 = box
    return Image.fromarray(img[y0:y1, x0:x1, :3]).resize(((x1 - x0) * scale, (y1 - y0) * scale), Image.NEAREST)


def sheet(name, box, scale, tags, labels):
    tiles = [crop(load(t, name), box, scale) for t in tags]
    w, h = tiles[0].size
    s = Image.new("RGB", (w * len(tiles) + 6 * (len(tiles) - 1), h), (255, 0, 255))
    for i, t in enumerate(tiles):
        s.paste(t, (i * (w + 6), 0))
    p = os.path.join(d, "crops", "%s_%s.png" % (name, "_".join(labels)))
    s.save(p)
    print("wrote", p)


sheet("hud3d", (0, 0, 280, 130), 3, ["off720", "on720"], ["off", "on"])
sheet("hud3d", (700, 0, 960, 130), 3, ["off720", "on720"], ["meter_off", "meter_on"])
sheet("hudonly", (0, 0, 280, 130), 3, ["off720", "on720", "off960"], ["off", "on", "native"])
sheet("hudonly", (30, 560 - 20 - 120 + 0, 330, 544), 3, ["off720", "on720", "off960"], ["cull_off", "cull_on", "cull_native"])
sheet("modelonly", (600, 90, 840, 270), 3, ["off720", "on720", "off960"], ["model_off", "model_on", "model_native"])
sheet("lines", (20, 130, 220, 330), 4, ["off720", "on720", "off960"], ["lines_off", "lines_on", "lines_native"])
# the full pictures too
for tag in ("off720", "on720"):
    Image.fromarray(load(tag, "hud3d")[:, :, :3]).save(os.path.join(d, "crops", "full_hud3d_%s.png" % tag))

print("FAILED: %d" % fails if fails else "PASS: all checks")
sys.exit(1 if fails else 0)

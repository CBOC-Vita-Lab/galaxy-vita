#!/usr/bin/env python3
# compositor25: compares the pictures of tests/vgx_fxcomp25_gl_test.cpp with the unrouted reference of their scene and checks the quality numbers.
#   fxcomp25_compare.py <prefix>      (reads <prefix>-<name>.rgba: two uint32 (w, h), then w*h RGBA bytes, first row at the bottom)
import sys, struct
import numpy as np

def load(prefix, name):
    with open(f"{prefix}-{name}.rgba", "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        return np.frombuffer(f.read(w * h * 4), np.uint8).reshape(h, w, 4)[:, :, :3].astype(np.float64)

def stats(a, b, mask=None):
    d = np.abs(a - b)
    if mask is not None: d = d[mask]
    mse = float(np.mean((a - b) ** 2)) if mask is None else float(np.mean((a[mask] - b[mask]) ** 2))
    psnr = 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    return float(d.max()), float(d.mean()), psnr

prefix = sys.argv[1]
fails = []
def check(ok, why):
    print(("PASS: " if ok else "FAIL: ") + why)
    if not ok: fails.append(why)

W, H = 960, 544
cols = np.arange(W)
band = np.zeros((H, W), bool)
# the depth edges of the ring scene: the pillar at NDC x -0.32 .. -0.18, the box at 0.30 .. 0.60 (pixels), rows well inside the picture
for x in (-0.32, -0.18, 0.30, 0.60):
    px = int(round((x + 1) / 2 * W))
    band[60:H - 60, px - 3:px + 4] = True

ref = load(prefix, "ring-ref")
l0, l1, q = load(prefix, "ring-l0"), load(prefix, "ring-l1"), load(prefix, "ring-q")
print("ring: whole picture / depth-edge band (+-3 px)    max   mean   PSNR")
rows = {}
for n, a in (("level 0", l0), ("level 1", l1), ("level 1, quarter", q)):
    m, me, p = stats(a, ref); mb, meb, pb = stats(a, ref, band)
    rows[n] = (me, meb)
    print(f"  {n:18s} whole {m:5.0f} {me:6.2f} {p:6.2f} dB | band {mb:5.0f} {meb:6.2f} {pb:6.2f} dB")
check(rows["level 1"][1] < rows["level 0"][1] * 0.8, f"nearest-depth composite lowers the error in the depth-edge band (mean {rows['level 0'][1]:.2f} -> {rows['level 1'][1]:.2f})")
check(stats(l1, ref)[2] > 28, "level 1 picture within 28 dB PSNR of the unrouted one")
check(stats(l0, ref)[2] > 25, "level 0 picture within 25 dB PSNR of the unrouted one")
cen = load(prefix, "ring-census")
check(np.array_equal(cen, ref), "census (=2) picture byte-identical to the unrouted picture")

# energy: the sum of the light, routed and not, on the glow-only scene
er, el = load(prefix, "energy-ref"), load(prefix, "energy-l0")
sr, sl = er.sum(), el.sum()
m, me, p = stats(el, er)
print(f"energy: unrouted sum {sr:.0f}, routed {sl:.0f} ({100 * (sl - sr) / sr:+.2f} %); max {m:.0f} mean {me:.2f} PSNR {p:.2f} dB")
check(abs(sl - sr) / sr < 0.03, "additive energy within 3 % (signed)")
check(p > 30, "glow-only scene within 30 dB PSNR")
# banding: the number of distinct levels along a horizontal line through the glow
line_r = er[H // 2, :, 0]; line_l = el[H // 2, :, 0]
print(f"banding: distinct red levels on the middle row: unrouted {len(np.unique(line_r))}, routed {len(np.unique(line_l))}")
check(len(np.unique(line_l)) >= 0.6 * len(np.unique(line_r)), "routed glow keeps at least 60 % of the unrouted gradient's levels")

# thin lines: 80 lines of 1.5 px: how much light they carry (sum over the picture minus the sheets' picture)
tr, ton, toff = load(prefix, "thin-ref"), load(prefix, "thin-on"), load(prefix, "thin-off")
base = load(prefix, "energy-ref")  # (the same glow sheets, no lines; wall differs: use the ring-free comparison below)
lr, lon, loff = tr.sum(), ton.sum(), toff.sum()
m1, me1, p1 = stats(ton, tr); m2, me2, p2 = stats(toff, tr)
print(f"thin: unrouted sum {lr:.0f}; thin lines kept at full size: {lon:.0f} (mean err {me1:.2f}, PSNR {p1:.2f}); routed with the thin test off: {loff:.0f} (mean err {me2:.2f}, PSNR {p2:.2f})")
# the lines' own light: compare the peak of the brightest pixels
pk = lambda a: float(np.percentile(a[:, :, 0], 99.9))
print(f"thin: 99.9th percentile red: unrouted {pk(tr):.0f}, thin test on {pk(ton):.0f}, thin test off {pk(toff):.0f}")
check(pk(ton) >= 0.9 * pk(tr), "with the thin test the lines stay as bright as unrouted (>= 90 % of the peak)")
check(pk(toff) < pk(ton) or me2 > me1, "without the thin test the lines are dimmer or the error larger (the defect it prevents)")

# static buffers and alpha
sr_, sl_ = load(prefix, "static-ref"), load(prefix, "static-l0")
m, me, p = stats(sl_, sr_); print(f"static sheets (fill measured from the kept copy): max {m:.0f} mean {me:.2f} PSNR {p:.2f} dB")
check(not np.array_equal(sl_, sr_), "static sheets were routed (picture differs from the unrouted one)")
check(p > 28, "static-sheet picture within 28 dB PSNR")
ar, aon, aoff, arr = (load(prefix, n) for n in ("alpha-ref", "alpha-on", "alpha-off", "alpharead-on"))
check(not np.array_equal(aon, ar), "alpha-writing sheets with no alpha reader are routed")
check(np.array_equal(aoff, ar), "with the alpha rule off the alpha-writing sheets stay at full size (identical)")
rr = load(prefix, "alpharead-on")
check(stats(aon, ar)[2] > 28, "alpha-writing routed picture within 28 dB PSNR")

# ---- round 2: the deferral and the size from the softness
def region(x0, x1, y0, y1):  # NDC -> pixel mask (rows from the bottom)
    m = np.zeros((H, W), bool)
    m[int((y0 + 1) / 2 * H):int((y1 + 1) / 2 * H), int((x0 + 1) / 2 * W):int((x1 + 1) / 2 * W)] = True
    return m
dr, don = load(prefix, "defer-ref"), load(prefix, "defer-on")
m, me, p = stats(don, dr)
print(f"defer: four layers in one deferred run vs unrouted: max {m:.0f} mean {me:.2f} PSNR {p:.2f} dB")
check(not np.array_equal(don, dr) and p > 28, "deferred run (one, additive-only scene) equals the unrouted picture up to the reduced-size error (>= 28 dB)")
er_, eon, efo = load(prefix, "deferover-ref"), load(prefix, "deferover-on"), load(prefix, "deferover-forced")
smoke = region(-0.35, 0.35, -0.35, 0.35)
_, me_on, p_on = stats(eon, er_, smoke); _, me_fo, p_fo = stats(efo, er_, smoke)
print(f"defer with a later over-blend: in the over-blend's rectangle: layer kept in place: mean err {me_on:.2f} ({p_on:.1f} dB); forced (negative control): mean err {me_fo:.2f} ({p_fo:.1f} dB)")
check(stats(eon, er_)[2] > 28, "over-blend overlapping: the layer kept in place, picture within 28 dB of the unrouted one")
check(me_fo > 1.5 * me_on, "negative control: forcing the deferral over the overlap shows the difference (glow added over the smoke)")
br, bon = load(prefix, "deferb-ref"), load(prefix, "deferb-on")
box = region(-0.25, 0.25, -0.35, 0.15)
_, me_in, _ = stats(bon, br, box); _, me_all, _ = stats(bon, br)
outside = ~box
_, me_out, _ = stats(bon, br, outside)
print(f"defer, opaque draw behind the layers (case b): error inside its rectangle {me_in:.2f}, outside {me_out:.2f}")
check(me_in > 3 * me_out, "case (b): the difference to the in-place order is confined to the opaque draw's rectangle (the deferred picture is the depth-correct one)")
cc = load(prefix, "defercap-cross")
check(stats(cc, dr)[2] > 28, "DEFER=2 (across a capture): one run, picture within 28 dB of the unrouted one")
ra = load(prefix, "ring-auto")
print(f"ring, size from softness (5 screens soft glow: quarter): whole PSNR {stats(ra, ref)[2]:.2f} dB, band mean {stats(ra, ref, band)[1]:.2f}")
check(stats(ra, ref)[2] > 28, "automatic quarter size on soft glow with the nearest-depth composite: within 28 dB")
print("FAILED" if fails else "ALL PASS")
sys.exit(1 if fails else 0)

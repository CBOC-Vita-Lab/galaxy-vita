#!/usr/bin/env python3
"""HOST REFERENCE of what the game does on the console with the player's own title screen (platform/include/port/title_art.h,
platform/include/port/title_art_png.h and vita/vgx-game/title_art_runtime.cpp). The public package carries no game artwork: the
first time the real title screen has been fully shown, the game saves the presented frame as ux0:data/smg/art/title.rgba
(960x544 RGBA) and makes from it, on the console:
  pic0.png    960x544  the title with its two text lines taken out;
  bg.png      840x500  the same, cut to size, softened and darkened where the LiveArea draws its gate and frames;
  startup.png 280x158  the title logo on the title's own sky, filling the gate picture within its margins, a light vignette;
  icon0.png   128x128  the logo on its sky, every logo pixel inside the round bubble's circle with a margin, darker towards the rim.
This script is the same algorithm step for step, in integers (NumPy and Python ints), with the same named constants, and the
console's quantizer through palette_png.quantize: for the same capture it gives the very same pixels and palette as the console
code (tests/test_title_art.py checks that on made-up captures, and on a real one when given). Use it to preview or to tune the
constants from a real capture of your own. Deterministic. The LiveArea Settings tile is not made here: the console keeps the
package's own settings.png.
  python3 -I tools/handheld/make_livearea_title.py <capture.rgba> <out-dir>
Needs NumPy. The neutral pictures the package ships: make_livearea.py."""
import importlib.util
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('palette_png', os.path.join(HERE, 'palette_png.py'))
palette_png = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(palette_png)

I64 = np.int64
kW, kH = 960, 544
# the capture's geometry and the outputs' (title_art.h, same names)
kTextY0, kTextY1, kTextX0, kTextX1 = 392, 518, 222, 738
kSkyY0 = 380
kLogoPad = 14
kLogoMaskRound = 12
kLogoMaskSoft = 40
kIconLogoRadius = 54
kIconFill = 96
kIconVignetteInner, kIconVignette = 600, 100
kGateMarginX, kGateMarginY = 16, 12
kGateVignetteInner, kGateVignette = 560, 70
kGateX0, kGateY0, kGateX1, kGateY1 = 262, 96, 578, 392
kCalmFeather = 96
kCalmKeep = 200
kCalmBlur = 3
kFrameBandY = 400
kFrameBandKeep = 190
kWeightBits = 14
kStarLift = 60
kTextLift = 150


def tdiv(a, b):
    """C's integer division (towards zero), for operands that can be negative."""
    q = np.abs(a) // np.abs(b)
    return np.where((a < 0) != (b < 0), -q, q)


def is_sky(px):
    px = px.astype(I64)
    return (px[..., 2] > px[..., 0] + 35) & (px[..., 2] + 25 > px[..., 1])


def find_logo_box(rgb):
    ns = ~is_sky(rgb)
    cols = ns[20:380].sum(axis=0)
    xs = np.nonzero(cols > 25)[0]
    if not len(xs):
        return None
    left, right = int(xs[0]), int(xs[-1])
    rows = ns[:, left:right + 1].sum(axis=1)
    ys = np.nonzero(rows[:390] > 25)[0]
    if not len(ys):
        return None
    return left, right, int(ys[0]), int(ys[-1])


def side_medians(rgb):
    """SideMedian for each text row: the median brightness of the row outside the text rectangle's columns."""
    side_mask = np.ones(kW, bool)
    side_mask[kTextX0:kTextX1] = False
    lum = rgb[kTextY0:kTextY1][:, side_mask].astype(I64).sum(axis=2)
    out = []
    for row in lum:
        cum = np.cumsum(np.bincount(row, minlength=766))
        out.append(int(np.nonzero(cum * 2 >= len(row))[0][0]))
    return np.array(out, I64)


def clean_title(rgb):
    """CleanTitle: the text lines out, the sky interpolated in, smoothed, the sky's stars given back."""
    rgb = rgb.astype(I64).copy()
    by0, by1, bx0, bx1 = kTextY0 - 4, kTextY1 + 4, kTextX0 - 4, kTextX1 + 4
    mask = np.zeros((kH, kW), np.uint8)
    reg = rgb[kTextY0:kTextY1, kTextX0:kTextX1]
    med = side_medians(rgb)[:, None]
    mask[kTextY0:kTextY1, kTextX0:kTextX1] = ((reg.sum(axis=2) > med + kTextLift) | (reg[..., 2] < reg[..., 0] + 30)).astype(np.uint8)
    rows = mask[by0:by1].copy()
    grown = np.zeros_like(rows)
    for k in range(-4, 5):
        grown[:, bx0:bx1] |= rows[:, bx0 + k:bx1 + k]
    mask[by0:by1, bx0:bx1] = grown[:, bx0:bx1]
    band = mask[by0:by1, bx0:bx1].copy()
    out = np.zeros_like(band)
    for k in range(-4, 5):
        lo, hi = max(0, -k), min(band.shape[0], band.shape[0] - k)
        out[lo:hi] |= band[lo + k:hi + k]
    mask[by0:by1, bx0:bx1] = out
    m = mask.astype(bool)
    px = rgb.tolist()
    mk = m.tolist()

    def end(ex, ey, vertical):
        vals = []
        for k in range(-2, 3):
            xx, yy = (ex + k, ey) if vertical else (ex, ey + k)
            if 0 <= xx < kW and 0 <= yy < kH and not mk[yy][xx]:
                vals.append(px[yy][xx])
        n = len(vals)
        return [sorted(v[c] for v in vals)[n // 2] for c in range(3)]

    for y in range(by0, by1):
        for x in range(bx0, bx1):
            if not mk[y][x]:
                continue
            yu, yd, xl, xr = y, y, x, x
            while mk[yu][x]:
                yu -= 1
            while mk[yd][x]:
                yd += 1
            while mk[y][xl]:
                xl -= 1
            while mk[y][xr]:
                xr += 1
            eu, ed, el, er = end(x, yu, True), end(x, yd, True), end(xl, y, False), end(xr, y, False)
            du, dd, dl, dr = y - yu, yd - y, x - xl, xr - x
            vs, hs = du + dd, dl + dr
            p = px[y][x]
            for c in range(3):
                v = (eu[c] * dd + ed[c] * du + vs // 2) // vs
                h = (el[c] * dr + er[c] * dl + hs // 2) // hs
                p[c] = (v * hs + h * vs + (vs + hs) // 2) // (vs + hs)
    rgb = np.array(px, I64)
    bh, bw = by1 - by0, bx1 - bx0
    ones = np.zeros((bh + 4, bw + 4), I64)
    ones[2:-2, 2:-2] = 1
    n = sum(ones[dy:dy + bh, dx:dx + bw] for dy in range(5) for dx in range(5))
    sel = m[by0:by1, bx0:bx1]
    for _ in range(3):
        copy = np.zeros((bh + 4, bw + 4, 3), I64)
        copy[2:-2, 2:-2] = rgb[by0:by1, bx0:bx1]
        s = sum(copy[dy:dy + bh, dx:dx + bw] for dy in range(5) for dx in range(5))
        new = (s + (n // 2)[..., None]) // n[..., None]
        region = rgb[by0:by1, bx0:bx1]
        region[sel] = new[sel]
    # stars: well above the median brightness of their own row (kStarLift)
    side_mask = np.ones(kW, bool)
    side_mask[kTextX0:kTextX1] = False
    side = rgb[kTextY0:kTextY1][:, side_mask]
    lum = side.sum(axis=2)
    medians = side_medians(rgb)
    is_star = lum > (np.array(medians, I64)[:, None] + kStarLift)
    bright = side[is_star]
    seen = len(bright)
    stars = bright[:4096]
    total = lum.size
    if not len(stars):
        return rgb.astype(np.uint8)
    threshold = seen * 4294967296 // total
    s = 1985

    def nxt():
        nonlocal s
        s ^= (s << 13) & 0xffffffff
        s ^= s >> 17
        s ^= (s << 5) & 0xffffffff
        return s

    starl = stars.tolist()
    for y in range(by0, by1):
        for x in range(bx0, bx1):
            if not mk[y][x]:
                continue
            if nxt() >= threshold:
                continue
            st = starl[nxt() % len(starl)]
            rgb[y, x] = np.maximum(rgb[y, x], st)
    return rgb.astype(np.uint8)


def cubic(num, den):
    num = abs(num)
    S = 65536
    t = num * S // den
    if t >= 2 * S:
        return 0
    if t < S:
        return (3 * t ** 3 - 5 * t * t * S + 2 * S ** 3) // (2 * S * S)
    return (-t ** 3 + 5 * t * t * S - 8 * t * S * S + 4 * S ** 3) // (2 * S * S)


def taps(in_size, out_size, first_out, count_out):
    """MakeTaps: per output, (first input sample, weights in 1/16384)."""
    big = max(in_size, out_size)
    q = 2 * big
    support = (2 * big + out_size - 1) // out_size + 1
    out = []
    for i in range(count_out):
        c2 = (2 * (first_out + i) + 1) * in_size
        xc = c2 // (2 * out_size)
        xmin, xmax = max(0, xc - support), min(in_size - 1, xc + support)
        raw = [cubic((2 * x + 1) * out_size - c2, q) for x in range(xmin, xmax + 1)]
        total = sum(raw)
        peak = 0
        for k in range(len(raw)):
            if raw[k] > raw[peak]:
                peak = k
        if total <= 0:
            w = [0] * len(raw)
            w[min(max(xc - xmin, 0), len(raw) - 1)] = 1 << kWeightBits
        else:
            w = [(r * (2 << kWeightBits) + total) // (2 * total) for r in raw]
            w[peak] += (1 << kWeightBits) - sum(w)
        out.append((xmin, np.array(w, I64)))
    return out


def weighted(acc):
    return np.clip((acc + (1 << (kWeightBits - 1))) >> kWeightBits, 0, 255)


def resample(src, full_w, full_h, ox, oy, ow, oh):
    """Resample: horizontal pass into 8 bits, then vertical (integers throughout)."""
    sh, sw = src.shape[:2]
    s = src.astype(I64)
    mid = np.zeros((sh, ow, 3), I64)
    for x, (first, w) in enumerate(taps(sw, full_w, ox, ow)):
        mid[:, x] = weighted((s[:, first:first + len(w)] * w[None, :, None]).sum(axis=1))
    out = np.zeros((oh, ow, 3), I64)
    for y, (first, w) in enumerate(taps(sh, full_h, oy, oh)):
        out[y] = weighted((mid[first:first + len(w)] * w[:, None, None]).sum(axis=0))
    return out.astype(np.uint8)


def cover(src, w, h):
    sh, sw = src.shape[:2]
    if w * sh >= h * sw:
        rw, rh = w, (2 * sh * w + sw) // (2 * sw)
    else:
        rh, rw = h, (2 * sw * h + sh) // (2 * sh)
    return resample(src, rw, rh, (rw - w) // 2, (rh - h) // 2, w, h)


def box_blur3(a, r):
    if r <= 0:
        return a
    a = a.astype(I64)
    h, w = a.shape
    n = 2 * r + 1
    for _ in range(3):
        idx = np.clip(np.arange(w)[:, None] + np.arange(-r, r + 1)[None, :], 0, w - 1)
        a = a[:, idx].sum(axis=2) // n
        idy = np.clip(np.arange(h)[:, None] + np.arange(-r, r + 1)[None, :], 0, h - 1)
        a = a[idy, :].sum(axis=1) // n
    return a


def logo_radius2(clean, box):
    L, R, T, B = box
    region = clean[T:B + 1, L:R + 1]
    ys, xs = np.nonzero(~is_sky(region))
    dx = 2 * (xs + L) + 1 - (L + R + 1)
    dy = 2 * (ys + T) + 1 - (T + B + 1)
    return int((dx.astype(I64) ** 2 + dy.astype(I64) ** 2).max()) if len(xs) else 0


def place_logo(clean, box, w, h, circle):
    dst = cover(clean[kSkyY0:], w, h).astype(I64)
    L, R, T, B = box
    x0, x1 = max(0, L - kLogoPad), min(kW, R + 1 + kLogoPad)
    y0, y1 = max(0, T - kLogoPad), min(kH, B + 1 + kLogoPad)
    bw, bh, lw0, lh0 = x1 - x0, y1 - y0, R - L + 1, B - T + 1
    if circle:
        num, den = 2 * kIconLogoRadius, math.isqrt(logo_radius2(clean, box)) + 1
        if num * lw0 > kIconFill * den:
            num, den = kIconFill, lw0
    else:
        aw, ah = w - 2 * kGateMarginX, h - 2 * kGateMarginY
        num, den = (aw, lw0) if aw * lh0 <= ah * lw0 else (ah, lh0)
    lw = max(1, (2 * bw * num + den) // (2 * den))
    lh = max(1, (2 * bh * num + den) // (2 * den))
    logo = resample(clean[y0:y1, x0:x1], lw, lh, 0, 0, lw, lh).astype(I64)
    radius = max(4, lw // kLogoMaskRound)
    xx = np.arange(lw)[None, :]
    yy = np.arange(lh)[:, None]
    dx = xx - np.clip(xx, radius, lw - 1 - radius)
    dy = yy - np.clip(yy, radius, lh - 1 - radius)
    mask = np.where(dx * dx + dy * dy <= radius * radius, 256, 0).astype(I64)
    mask = box_blur3(mask, max(1, lw // kLogoMaskSoft))
    px, py = (w - lw) // 2, (h - lh) // 2
    ox0, oy0, ox1, oy1 = max(0, px), max(0, py), min(w, px + lw), min(h, py + lh)
    d = dst[oy0:oy1, ox0:ox1]
    l = logo[oy0 - py:oy1 - py, ox0 - px:ox1 - px]
    mm = mask[oy0 - py:oy1 - py, ox0 - px:ox1 - px][..., None]
    dst[oy0:oy1, ox0:ox1] = np.clip(d + ((l - d) * mm + 128) // 256, 0, 255)
    return dst.astype(np.uint8)


def vignette(img, inner, strength):
    h, w = img.shape[:2]
    x = np.arange(w, dtype=I64)[None, :]
    y = np.arange(h, dtype=I64)[:, None]
    dx = tdiv((2 * x + 1 - w) * 1024, np.int64(w))
    dy = tdiv((2 * y + 1 - h) * 1024, np.int64(h))
    d2 = dx * dx + dy * dy
    in2, full = inner * inner, 1024 * 1024
    f = np.minimum(256, np.maximum(d2 - in2, 0) * 256 // (full - in2))
    f = f * f // 256
    k = np.where(d2 <= in2, 256, 256 - f * strength // 256)
    return ((img.astype(I64) * k[..., None] + 128) >> 8).astype(np.uint8)


def calm_zones(img):
    h, w = img.shape[:2]
    p = img.astype(I64)
    blurred = np.stack([box_blur3(p[..., c], kCalmBlur) for c in range(3)], axis=2)
    x = np.arange(w, dtype=I64)[None, :]
    y = np.arange(h, dtype=I64)[:, None]
    dx = np.maximum(np.maximum(kGateX0 - x, x - kGateX1), 0)
    dy = np.maximum(np.maximum(kGateY0 - y, y - kGateY1), 0)
    d2 = dx * dx + dy * dy
    d = np.array([math.isqrt(int(v)) for v in d2.ravel()], I64).reshape(d2.shape)
    t = np.where(d >= kCalmFeather, 0, 256 - d * 256 // kCalmFeather)
    t = t * t * (768 - 2 * t) // 65536
    k = 256 - t * (256 - kCalmKeep) // 256
    yb = np.broadcast_to(y, k.shape)
    band = 256 - (yb - kFrameBandY) * (256 - kFrameBandKeep) // (h - kFrameBandY)
    k = np.where(yb >= kFrameBandY, k * band // 256, k)
    v = p + ((blurred - p) * t[..., None] + 128) // 256
    return np.clip((v * k[..., None] + 128) >> 8, 0, 255).astype(np.uint8)


def build(rgba):
    """The four pictures as (name, rgb) from a 960x544 RGBA capture, or None when it has no logo."""
    rgb = np.asarray(rgba, np.uint8).reshape(kH, kW, 4)[..., :3]
    clean = clean_title(rgb)
    box = find_logo_box(clean)
    if box is None:
        return None
    return [('pic0', clean), ('bg', calm_zones(cover(clean, 840, 500))),
            ('startup', vignette(place_logo(clean, box, 280, 158, False), kGateVignetteInner, kGateVignette)),
            ('icon0', vignette(place_logo(clean, box, 128, 128, True), kIconVignetteInner, kIconVignette))]


def main(argv):
    src, outdir = argv
    data = open(src, 'rb').read()
    if len(data) != kW * kH * 4:
        sys.exit('a capture is 960x544 RGBA')
    pictures = build(np.frombuffer(data, np.uint8))
    if pictures is None:
        sys.exit('no logo found in the capture')
    os.makedirs(outdir, exist_ok=True)
    for name, rgb in pictures:
        idx, pal = palette_png.quantize(rgb)
        with open(os.path.join(outdir, name + '.png'), 'wb') as f:
            f.write(palette_png.png8(idx, pal))
        print(name, rgb.shape[1], 'x', rgb.shape[0])


if __name__ == '__main__':
    main(sys.argv[1:])

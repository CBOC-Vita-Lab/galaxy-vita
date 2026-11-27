#!/usr/bin/env python3
"""Draws the Vita package's bubble and LiveArea pictures into vita/vgx-game/sce_sys/ (or the folder given as the first argument):
icon0.png 128x128, pic0.png 960x544, livearea/contents/bg.png 840x500, startup.png 280x158 and settings.png 280x64.

All of it is original artwork drawn here in code: a deep-space scene (nebula clouds, a starfield in three depths and an abstract
centre-piece: a ringed star cluster with orbit lines (the default; orbit2: an uneven ring and a wider cluster), a spiral galaxy or a
comet arc, depending on --style) and the
game's name as plain text (no other words) set in
Inter (SIL Open Font License 1.1; the letter outlines are in livearea_glyphs.py, see licenses/Inter-OFL.txt). No game artwork: no
characters, no logo or wordmark, nothing taken or traced from the game. On a player's console the game replaces the bubble, LiveArea
background and start pictures with ones made from the player's own title screen (docs/ASSETS.md; host reference:
make_livearea_title.py).

Reproducible to the byte on any machine: every pixel is computed with integer arithmetic (NumPy int64 and Python ints: fixed-point
noise, an integer sine table, an integer arctangent, exact integer square roots, box blurs as running sums), the text is set by our
own scanline rasteriser from fixed polygons (no FreeType), the 256 colours are chosen and dithered by palette_png.quantize (the
console's own median cut and Floyd-Steinberg, ported exactly) and the PNG is compressed by palette_png's own deflate (zlib and zlib-ng
give different bytes). Pillow is not used. All pictures are 8-bit palette PNGs with a 256-entry palette, no transparency and no
interlace, like the files the package has always shipped (the console's installer refuses other kinds).
  python3 tools/handheld/make_livearea.py [out-dir] [--style orbit|orbit2|spiral|comet]
Needs NumPy. tools/handheld/check_livearea.py checks the result; livearea_preview.py draws contact sheets for choosing a style."""
import importlib.util
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


palette_png = _load('palette_png')
GLYPHS = _load('livearea_glyphs')

STYLES = ('orbit', 'orbit2', 'spiral', 'comet')
DEFAULT_STYLE = 'orbit'
ONE = 4096  # fixed-point one for weights and intensities
I64 = np.int64

# ------------------------------------------------------------------------------------------------ integer maths


def _sine_table():
    """sin over one turn in 4096 steps, scaled by 4096 (Bhaskara's rational form: within 0.2 % of the sine, exact integers)."""
    half = 2048
    out = []
    for x in range(4096):
        a = x % half
        u = a * (half - a)
        s = 16 * u * ONE // (5 * half * half - 4 * u)
        out.append(s if x < half else -s)
    return np.array(out, I64)


SIN = _sine_table()


def isin(turn65536):
    return SIN[(turn65536 >> 4) & 4095]


def icos(turn65536):
    return SIN[((turn65536 >> 4) + 1024) & 4095]


def isqrt(a):
    a = np.maximum(a, 0).astype(I64)
    r = np.sqrt(a.astype(np.float64)).astype(I64)  # a first guess only; corrected to the exact integer root below
    r = np.where(r * r > a, r - 1, r)
    r = np.where(r * r > a, r - 1, r)
    r = np.where((r + 1) * (r + 1) <= a, r + 1, r)
    return np.where((r + 1) * (r + 1) <= a, r + 1, r)


def iatan2(y, x):
    """Angle of (x, y) in 1/65536 turns, 0..65535 (a rational fit of the arctangent per octant, within 0.03 degrees)."""
    ax, ay = np.abs(x), np.abs(y)
    mx, mn = np.maximum(ax, ay), np.minimum(ax, ay)
    z = mn * 65536 // np.maximum(mx, 1)
    a = (8192 * z + (z * (65536 - z) // 65536) * (2552 + 691 * z // 65536) // 65536) // 65536
    a = np.where(ay > ax, 16384 - a, a)
    a = np.where(x < 0, 32768 - a, a)
    a = np.where(y < 0, 65536 - a, a)
    return a & 65535


def _hash(ix, iy, seed):
    h = (ix.astype(np.uint64) * np.uint64(0x9E3779B1) + iy.astype(np.uint64) * np.uint64(0x85EBCA77) + np.uint64(seed * 0x27D4EB2F + 1)) & np.uint64(0xffffffff)
    h ^= h >> np.uint64(15)
    h = (h * np.uint64(0x2C1B3C6D)) & np.uint64(0xffffffff)
    h ^= h >> np.uint64(12)
    h = (h * np.uint64(0x297A2D39)) & np.uint64(0xffffffff)
    h ^= h >> np.uint64(15)
    return (h & np.uint64(0xffff)).astype(I64)


def noise(X, Y, cell, seed):
    """Value noise 0..65535 on a lattice of `cell` world units, smoothstep-interpolated in fixed point."""
    ix, iy = X // cell, Y // cell
    tx, ty = (X - ix * cell) * ONE // cell, (Y - iy * cell) * ONE // cell
    sx = tx * tx * (3 * ONE - 2 * tx) // (ONE * ONE)
    sy = ty * ty * (3 * ONE - 2 * ty) // (ONE * ONE)
    a, b = _hash(ix, iy, seed), _hash(ix + 1, iy, seed)
    c, d = _hash(ix, iy + 1, seed), _hash(ix + 1, iy + 1, seed)
    top = a + (b - a) * sx // ONE
    bottom = c + (d - c) * sx // ONE
    return top + (bottom - top) * sy // ONE


def fbm(X, Y, cell, seed, octaves=5):
    total, weight, amp = 0, 0, 1 << octaves
    for o in range(octaves):
        total = total + noise(X, Y, max(2, cell >> o), seed + o * 101) * amp
        weight += amp
        amp >>= 1
    return total // weight


def ramp(v, lo, hi):
    """0 below lo, ONE above hi, linear between (all integers)."""
    return np.clip((v - lo) * ONE // max(1, hi - lo), 0, ONE)


def smooth(t):
    return t * t * (3 * ONE - 2 * t) // (ONE * ONE)


def blur(a, r, passes=3):
    """Box blur of radius r along both axes, `passes` times (about a Gaussian), as exact running sums; edges repeat."""
    if r <= 0:
        return a
    a = a.astype(I64)
    for _ in range(passes):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)] + [(0, 0)] * (a.ndim - 2)
            pad[axis] = (r + 1, r)
            p = np.pad(a, pad, mode='edge')
            c = np.cumsum(p, axis=axis)
            n = a.shape[axis]
            hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
            lo = np.take(c, np.arange(0, n), axis=axis)
            a = (hi - lo) // (2 * r + 1)
    return a


class Rand:
    """xorshift32: the same sequence everywhere."""

    def __init__(self, seed):
        self.s = (seed * 2654435761 + 12345) & 0xffffffff or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xffffffff
        s ^= s >> 17
        s ^= (s << 5) & 0xffffffff
        self.s = s
        return s

    def below(self, n):
        return self.next() % n


# ------------------------------------------------------------------------------------------------ text


def layout(text, face, size, tracking=0):
    """Polygons of `text` at `size` px (em), pen at (0, 0) on the baseline, in 1/32 px, and the advance width in 1/32 px.
    `tracking` is extra space between letters in 1/1000 em."""
    f = GLYPHS.FACES[face]
    polys, pen = [], 0
    scale = size * 32
    for i, ch in enumerate(text):
        adv, contours = f['glyphs'][ch]
        for c in contours:
            xs = [pen + (c[k] * scale) // GLYPHS.UNITS for k in range(0, len(c), 2)]
            ys = [-(c[k + 1] * scale) // GLYPHS.UNITS for k in range(0, len(c), 2)]
            polys.append((xs, ys))
        kern = f['kern'].get(text[i:i + 2], 0) if i + 1 < len(text) else 0
        pen += ((adv + kern) * scale) // GLYPHS.UNITS
        if i + 1 < len(text):
            pen += tracking * scale // 1000
    return polys, pen


def text_width(text, face, size, tracking=0):
    return layout(text, face, size, tracking)[1] // 32


def rasterise(polys, w, h, ox32, oy32):
    """Coverage 0..256 (16 x 16 samples per pixel, non-zero winding) of the polygons moved by (ox32, oy32), on a w x h grid."""
    x0s, y0s, x1s, y1s = [], [], [], []
    for xs, ys in polys:
        n = len(xs)
        for k in range(n):
            x0s.append(xs[k] + ox32)
            y0s.append(ys[k] + oy32)
            x1s.append(xs[(k + 1) % n] + ox32)
            y1s.append(ys[(k + 1) % n] + oy32)
    x0, y0, x1, y1 = (np.array(v, I64) for v in (x0s, y0s, x1s, y1s))
    keep = y0 != y1
    x0, y0, x1, y1 = x0[keep], y0[keep], x1[keep], y1[keep]
    direction = np.where(y1 > y0, 1, -1)
    ymin, ymax = np.minimum(y0, y1), np.maximum(y0, y1)
    # sub-rows j sample at y = (2j + 1) / 2 in 1/32 px, i.e. the centres of 1/16 px rows
    j0 = -((1 - ymin) // 2)
    j1 = -((1 - ymax) // 2)
    j0 = np.clip(j0, 0, h * 16)
    j1 = np.clip(j1, 0, h * 16)
    counts = np.maximum(j1 - j0, 0)
    edge = np.repeat(np.arange(len(x0)), counts)
    start = np.repeat(j0, counts)
    offsets = np.arange(counts.sum()) - np.repeat(np.cumsum(counts) - counts, counts)
    row = start + offsets
    ys = 2 * row + 1
    ex0, ey0, ex1, ey1 = x0[edge], y0[edge], x1[edge], y1[edge]
    xs = ex0 + (ys - ey0) * (ex1 - ex0) // (ey1 - ey0)
    col = -((1 - xs) // 2)  # first sub-column whose centre (2k + 1) / 2 is at or right of x
    order = np.lexsort((col, row))
    row, col, wind = row[order], col[order], direction[edge][order]
    acc = np.cumsum(wind)
    inside = acc[:-1] != 0
    same = row[:-1] == row[1:]
    sel = np.nonzero(inside & same)[0]
    cov = np.zeros((h, w), I64)
    if not len(sel):
        return cov
    r, a, b = row[sel], np.clip(col[sel], 0, w * 16), np.clip(col[sel + 1], 0, w * 16)
    diff = np.zeros((h * 16, w * 16 + 1), np.int32)
    np.add.at(diff, (r, a), 1)
    np.add.at(diff, (r, b), -1)
    sub = np.cumsum(diff[:, :w * 16], axis=1)
    return (sub.reshape(h, 16, w, 16) > 0).sum(axis=(1, 3)).astype(I64)


def draw_text(img, text, face, size, x, baseline, colour, tracking=0, glow=None, shadow=None, gradient=None):
    """Sets `text` with its left edge at x and its baseline at `baseline` (pixels) into img (h x w x 3, 1/256 units).
    glow = (radius, rgb, strength 0..ONE); shadow = (dy, radius, strength 0..ONE); gradient = rgb at the bottom of the capitals."""
    h, w = img.shape[:2]
    polys, _ = layout(text, face, size, tracking)
    # rasterise only the text's box (plus room for its glow and shadow), then place it
    allx = [v for xs, _ in polys for v in xs]
    ally = [v for _, ys in polys for v in ys]
    m = 2 + (3 * glow[0] if glow else 0) + (3 * shadow[1] + shadow[0] if shadow else 0)
    bx0, by0 = max(0, x + min(allx) // 32 - m), max(0, baseline + min(ally) // 32 - m)
    bx1, by1 = min(w, x + max(allx) // 32 + m + 2), min(h, baseline + max(ally) // 32 + m + 2)
    box = rasterise(polys, bx1 - bx0, by1 - by0, (x - bx0) * 32, (baseline - by0) * 32) * ONE // 256
    cov = np.zeros((h, w), I64)
    cov[by0:by1, bx0:bx1] = box
    if shadow:
        dy, r, k = shadow
        s = blur(np.roll(cov, dy, axis=0), r) * k // ONE
        img[:] = img * (ONE - s[..., None]) // ONE
    if glow:
        r, rgb, k = glow
        g = blur(cov, r) * k // ONE
        img += g[..., None] * (np.array(rgb, I64) * 256)[None, None, :] // ONE
    fill = np.array(colour, I64) * 256
    if gradient is not None:
        cap = GLYPHS.FACES[face]['cap'] * size // GLYPHS.UNITS
        t = np.clip((np.arange(h, dtype=I64) - (baseline - cap)) * ONE // max(1, cap), 0, ONE)[:, None, None]
        fill = fill[None, None, :] + (np.array(gradient, I64)[None, None, :] * 256 - fill[None, None, :]) * t // ONE
    img[:] = img + (fill - img) * cov[..., None] // ONE


# ------------------------------------------------------------------------------------------------ the scene

class Camera:
    """Picture pixel (px, py) -> world (X, Y): the picture centre looks at (cx, cy), `units` world units per pixel (num/den)."""

    def __init__(self, w, h, cx, cy, num, den=1):
        self.w, self.h, self.cx, self.cy, self.num, self.den = w, h, cx, cy, num, den

    def grid(self):
        px = np.arange(self.w, dtype=I64)[None, :]
        py = np.arange(self.h, dtype=I64)[:, None]
        X = self.cx + (2 * px + 1 - self.w) * self.num // (2 * self.den)
        Y = self.cy + (2 * py + 1 - self.h) * self.num // (2 * self.den)
        return np.broadcast_to(X, (self.h, self.w)), np.broadcast_to(Y, (self.h, self.w))

    def to_pixel(self, X, Y):
        """World point to picture pixel, in 1/16 px."""
        return ((X - self.cx) * 16 * self.den // self.num + self.w * 8, (Y - self.cy) * 16 * self.den // self.num + self.h * 8)


def add(img, light, rgb):
    """Adds light (0..ONE or more) of colour rgb."""
    img += light[..., None] * (np.array(rgb, I64) * 256)[None, None, :] // ONE


def mul(img, factor):
    img[:] = img * factor[..., None] // ONE


PALETTES = {
    # base top, base bottom, nebula A, nebula B, dust, core, disk, knots, accent (text glow / outline)
    'spiral': dict(top=(2, 4, 16), bottom=(10, 8, 30), neb_a=(24, 120, 150), neb_b=(120, 36, 132), lift=(30, 24, 70),
                   core=(255, 222, 168), disk=(132, 168, 255), knots=(255, 96, 176), accent=(110, 180, 255), title=(255, 255, 255),
                   title_low=(196, 214, 255), sub=(176, 196, 236), edge=(150, 200, 255)),
    'comet': dict(top=(6, 2, 16), bottom=(22, 6, 30), neb_a=(112, 30, 96), neb_b=(150, 72, 44), lift=(50, 18, 44),
                  core=(255, 246, 228), disk=(140, 220, 255), knots=(255, 170, 90), accent=(255, 150, 110), title=(255, 250, 244),
                  title_low=(255, 214, 190), sub=(240, 196, 200), edge=(255, 176, 120)),
    'orbit': dict(top=(1, 8, 16), bottom=(4, 18, 30), neb_a=(12, 92, 104), neb_b=(20, 52, 124), lift=(8, 42, 60),
                  core=(236, 250, 255), disk=(110, 230, 230), knots=(255, 210, 120), accent=(120, 240, 230), title=(242, 255, 255),
                  title_low=(170, 236, 236), sub=(150, 210, 214), edge=(120, 236, 220)),
}


class Scene:
    def __init__(self, style):
        self.uneven = style == 'orbit2'          # orbit2 is orbit with a different centre-piece; everything else is the same
        self.style = 'orbit' if self.uneven else style
        self.p = PALETTES[self.style]

    # ---- background and nebula (world units: about 4 per pixel of pic0)
    def sky(self, cam, centre_px=None):
        h, w = cam.h, cam.w
        X, Y = cam.grid()
        p = self.p
        t = (np.arange(h, dtype=I64) * ONE // max(1, h - 1))[:, None]
        img = np.zeros((h, w, 3), I64)
        for c in range(3):
            img[..., c] = (p['top'][c] * 256 + (p['bottom'][c] - p['top'][c]) * 256 * t // ONE)
        # warped clouds: two colours, each in its own part of the sky, with darker dust between
        wx = fbm(X + 7000, Y - 3000, 1400, 11, 3) - 32768
        wy = fbm(X - 5000, Y + 9000, 1400, 23, 3) - 32768
        WX, WY = X + wx // 24, Y + wy // 24
        a = fbm(WX, WY, 1800, 31, 5)
        b = fbm(WX + 20000, WY - 12000, 1500, 47, 5)
        dust = fbm(X - 9000, Y + 4000, 700, 59, 4)
        if self.style == 'spiral':
            band = self._band(X, Y, 120, -1700, 1300)        # a diagonal band of clouds through the galaxy
            side = ramp(X + Y // 2, -2600, 2600)               # teal on the left, violet on the right
        elif self.style == 'comet':
            band = self._band(X, Y, -260, 600, 1800)
            side = ramp(X - Y, -3000, 3000)
        else:
            band = self._band(X, Y, 40, 300, 2200)
            side = ramp(Y - X // 3, -2400, 2400)
        # (these fractal sums gather between about 10000 and 36000: the ramps below are set on that range)
        fine = fbm(WX * 3, WY * 3, 900, 37, 4)                 # filaments inside the clouds
        texture = ONE * 6 // 10 + fine // 40
        da = smooth(ramp(a, 20000, 36000)) * band // ONE * texture // ONE
        db = smooth(ramp(b, 20000, 36000)) * band // ONE * texture // ONE
        strength = ONE if self.style == 'spiral' else ONE * 3 // 4
        add(img, da * (ONE - side) // ONE * strength // ONE, p['neb_a'])
        add(img, db * side // ONE * strength // ONE, p['neb_b'])
        add(img, smooth(ramp(a + b, 50000, 72000)) * band // ONE * 2 // 3, p['lift'])  # bright cores of the clouds
        add(img, (ONE - band) * smooth(ramp(a, 14000, 34000)) // ONE // 5, p['neb_b'] if self.style != 'orbit' else p['neb_a'])
        mul(img, ONE - smooth(ramp(dust, 24000, 34000)) * 7 // 10)
        return img, X, Y

    def _band(self, X, Y, angle_deg, offset, width):
        """1 along a straight band through the sky (angle in degrees, distance from the origin, half width), 0 far from it."""
        turn = angle_deg * 65536 // 360
        d = (X * isin(turn) - Y * icos(turn)) // ONE - offset
        return ONE - smooth(ramp(np.abs(d), width // 3, width * 2))

    # ---- the centre-pieces, in world units around (gx, gy)
    def spiral(self, img, X, Y, gx, gy, radius, k=ONE):
        p = self.p
        dx, dy = X - gx, Y - gy
        rot = 335 * 65536 // 360
        C, S = icos(rot), isin(rot)
        u = (dx * C + dy * S) // ONE
        v = (-dx * S + dy * C) // ONE
        vt = v * 1000 // 470                                    # the disc seen at a slant
        r = isqrt(u * u + vt * vt)
        theta = iatan2(vt, u)
        rr = r * 1000 // radius                                 # 0..1000 at the rim
        # two arms, each wound about 1.4 turns from the bulge to the rim (phase in 1/65536 turn)
        wind = isqrt(rr * 1000) * 190                           # winds faster near the centre, like a logarithmic spiral
        phase = (2 * theta - wind) & 65535
        arm = (icos(phase) + ONE) // 2
        arm = arm * arm // ONE
        arm = arm * arm // ONE * arm // ONE
        lane = (icos((phase - 7000) & 65535) + ONE) // 2        # dust on the inner edge of each arm
        lane = lane * lane // ONE
        lane = lane * lane // ONE
        env = ONE * 230 * 230 // (230 * 230 + rr * rr)          # brightness falls off outwards
        rim = smooth(ramp(1000 - rr, 0, 520))
        start = smooth(ramp(rr, 60, 220))                       # the arms begin outside the bulge
        clump = fbm(X * 4, Y * 4, 260, 71, 3)
        m = ONE * 6 // 10 + clump // 24
        disk = env * rim // ONE * (ONE // 6 + arm * 5 // 6 * start // ONE) // ONE * m // ONE
        knots = ramp(clump, 31000, 41000) * arm // ONE * start // ONE * rim // ONE
        knots = knots * knots // ONE
        rb = isqrt(u * u + (v * 1000 // 700) ** 2) * 1000 // radius  # the bulge is rounder than the disc
        core = ONE * 70 * 70 // (70 * 70 + rb * rb)
        bulge = ONE * 170 * 170 // (170 * 170 + rb * rb)
        halo = ONE * 520 * 520 // (520 * 520 + rr * rr) * rim // ONE
        mul(img, ONE - lane * start // ONE * env // ONE * rim // ONE * 45 // 100)
        add(img, halo * k // ONE // 3, p['lift'])
        add(img, halo * halo // ONE * k // ONE // 4, p['disk'])
        add(img, disk * 13 // 5 * k // ONE, p['disk'])
        add(img, disk * env // ONE * 6 // 5 * k // ONE, p['core'])   # the inner disc is warmer
        add(img, knots * 7 // 5 * k // ONE, p['knots'])
        add(img, bulge * bulge // ONE * 3 // 2 * k // ONE, p['core'])
        add(img, core * core // ONE * 3 * k // ONE, (255, 250, 236))

    def comet(self, img, X, Y, gx, gy, radius, k=ONE):
        """A comet on a wide arc: the head at (gx, gy), the tail curving back along a circle of `radius`, fading."""
        p = self.p
        # the arc's centre lies below-left of the head; the tail runs anticlockwise from the head
        acx, acy = gx - radius * 3 // 10, gy + radius * 95 // 100
        dx, dy = X - acx, Y - acy
        r = isqrt(dx * dx + dy * dy)
        ang = iatan2(dy, dx)
        head_ang = int(iatan2(np.array([gy - acy]), np.array([gx - acx]))[0])
        along = (head_ang - ang) & 65535                        # how far back along the tail (1/65536 turn)
        span = 12500
        t = np.clip(ONE - along * ONE // span, 0, ONE)          # 1 at the head, 0 at the end of the tail
        t = np.where(along > 40000, 0, t)
        width = 30 + (ONE - t) * 260 // ONE                      # the tail widens behind the head
        off = np.abs(r - radius)
        across = ONE * width * width // (width * width + off * off * 4)
        ion = ONE * 18 * 18 // (18 * 18 + (r - radius + 25) ** 2 * 4)
        dust = fbm(X * 2, Y * 2, 500, 83, 3)
        tail = across * t // ONE * t // ONE * (ONE // 2 + dust // 32) // ONE
        hx, hy = X - gx, Y - gy
        d2 = hx * hx + hy * hy
        head = ONE * 60 * 60 // (60 * 60 + d2)
        bloom = ONE * 300 * 300 // (300 * 300 + d2)
        add(img, tail * 9 // 5 * k // ONE, p['neb_b'])
        add(img, ion * t // ONE * k // ONE, p['disk'])
        add(img, tail * t // ONE * k // ONE, p['core'])
        add(img, bloom * k // ONE // 2, p['knots'])
        add(img, head * 4 * k // ONE, p['core'])

    def orbit(self, img, X, Y, gx, gy, radius, k=ONE, rings=True):
        """A bright young star cluster in a glowing ring of gas, with thin orbit lines around it."""
        p = self.p
        dx, dy = X - gx, Y - gy
        rot = 20 * 65536 // 360
        C, S = icos(rot), isin(rot)
        u = (dx * C + dy * S) // ONE
        v = (-dx * S + dy * C) // ONE
        vt = v * 1000 // 520
        r = isqrt(u * u + vt * vt)
        rr = r * 1000 // radius
        if self.uneven:  # orbit2: an uneven ring, lit more on one side, and a wider cluster of 12 stars
            theta = iatan2(vt, u)
            lit = ONE * 2 // 5 + (icos((theta - 9000) & 65535) + ONE) * 3 // 10
            ring = ONE * 42 * 42 // (42 * 42 + (rr - 430 + (fbm(X, Y, 700, 93, 2) - 24000) // 400) ** 2)
            ring = ring * (ONE // 5 + fbm(X * 3, Y * 3, 400, 91, 3) // 16) // ONE * lit // ONE
            cluster = [(0, 0, 6), (-210, -70, 4), (240, 60, 5), (90, -260, 3), (-140, 230, 3), (330, -150, 3), (-340, 40, 3),
                       (40, 170, 2), (-60, -170, 2), (160, -40, 2), (-260, -210, 2), (300, 220, 2)]
        else:  # orbit (the owner's choice): an even ring and eight stars inside it
            ring = ONE * 42 * 42 // (42 * 42 + (rr - 430) ** 2)
            ring = ring * (ONE // 4 + fbm(X * 3, Y * 3, 400, 91, 3) // 20) // ONE
            cluster = [(0, 0, 6), (-130, -50, 3), (150, 40, 4), (60, -170, 3), (-90, 150, 3), (210, -100, 2), (-220, 30, 2), (30, 120, 2)]
        inner = ONE * 260 * 260 // (260 * 260 + rr * rr) * 2 // 3
        add(img, ring * 2 * k // ONE, p['disk'])
        add(img, ring * ring // ONE * k // ONE, p['core'])
        add(img, inner * k // ONE // 2, p['neb_a'])
        if rings:
            for target, weight in ((700, 3), (930, 2)):
                line = ONE * 4 * 4 // (4 * 4 + (rr - target) ** 2)
                add(img, line * weight // 4 * k // ONE, p['edge'])
        for cxp, cyp, sz in cluster:  # (positions in units of the radius)
            sx, sy = dx - cxp * radius // 1000, dy - cyp * radius // 1000
            d2 = sx * sx + sy * sy
            size = sz * radius // 160
            add(img, ONE * size * size // (size * size + d2) * 2 * k // ONE, p['core'])

    def piece(self, img, X, Y, gx, gy, radius, k=ONE):
        getattr(self, self.style)(img, X, Y, gx, gy, radius, k)

    # ---- stars
    def stars(self, img, cam, seed, density, bright=6, band=None, hero_scale=ONE):
        """Three depths: faint dust (sub-pixel), small stars, and a few bright ones with a glow and four thin rays."""
        h, w = img.shape[:2]
        rnd = Rand(seed)
        tint = [(200, 218, 255), (255, 252, 244), (255, 226, 190), (190, 240, 255), (255, 214, 236)]
        # faint: bilinear single pixels
        n = w * h // density
        xs = np.array([rnd.below(w * 16) for _ in range(n)], I64)
        ys = np.array([rnd.below(h * 16) for _ in range(n)], I64)
        bs = np.array([40 + rnd.below(70) for _ in range(n)], I64)
        cs = np.array([tint[rnd.below(3)] for _ in range(n)], I64).reshape(-1, 3)
        if band is not None:
            keep = band[np.clip(ys // 16, 0, h - 1), np.clip(xs // 16, 0, w - 1)] + ONE // 3 > np.array([rnd.below(ONE) for _ in range(n)], I64)
            xs, ys, bs, cs = xs[keep], ys[keep], bs[keep], cs[keep]
        fx, fy = xs % 16, ys % 16
        x0, y0 = xs // 16, ys // 16
        for ox, oy, wgt in ((0, 0, (16 - fx) * (16 - fy)), (1, 0, fx * (16 - fy)), (0, 1, (16 - fx) * fy), (1, 1, fx * fy)):
            xx, yy = np.clip(x0 + ox, 0, w - 1), np.clip(y0 + oy, 0, h - 1)
            for c in range(3):
                np.add.at(img[..., c], (yy, xx), bs * wgt * cs[:, c] // 256)
        # small: a soft disc a pixel or two across
        n2 = w * h // (density * 9)
        for _ in range(n2):
            x, y = rnd.below(w * 16), rnd.below(h * 16)
            b = 110 + rnd.below(140)
            c = tint[rnd.below(5)]
            self._disc(img, x, y, b * ONE // 255, c, 10 + rnd.below(8))
        # bright: glow and rays
        for _ in range(bright):
            x, y = 32 + rnd.below(w * 16 - 64), 32 + rnd.below(h * 16 - 64)
            c = tint[rnd.below(5)]
            self._hero(img, x, y, (ONE * 3 // 4 + rnd.below(ONE // 2)) * hero_scale // ONE, c, 4 + rnd.below(6))

    def _disc(self, img, x16, y16, b, rgb, size16):
        h, w = img.shape[:2]
        cx, cy = x16 // 16, y16 // 16
        r = 3
        y0, y1, x0, x1 = max(0, cy - r), min(h, cy + r + 1), max(0, cx - r), min(w, cx + r + 1)
        if y0 >= y1 or x0 >= x1:
            return
        yy = (np.arange(y0, y1, dtype=I64)[:, None] * 16 + 8 - y16)
        xx = (np.arange(x0, x1, dtype=I64)[None, :] * 16 + 8 - x16)
        d2 = xx * xx + yy * yy
        s2 = size16 * size16
        light = b * s2 // (s2 + d2 * 3)
        light = light * light // ONE
        img[y0:y1, x0:x1] += light[..., None] * (np.array(rgb, I64) * 256)[None, None, :] // ONE

    def _hero(self, img, x16, y16, b, rgb, rays):
        h, w = img.shape[:2]
        cx, cy = x16 // 16, y16 // 16
        L = rays * 4 + 6
        y0, y1, x0, x1 = max(0, cy - L), min(h, cy + L + 1), max(0, cx - L), min(w, cx + L + 1)
        if y0 >= y1 or x0 >= x1:
            return
        yy = (np.arange(y0, y1, dtype=I64)[:, None] * 16 + 8 - y16)
        xx = (np.arange(x0, x1, dtype=I64)[None, :] * 16 + 8 - x16)
        d2 = xx * xx + yy * yy
        core = ONE * 400 // (400 + d2)
        glow = ONE * 4000 // (4000 + d2) // 4
        L16 = L * 16
        ax, ay = np.abs(xx), np.abs(yy)
        rx = np.clip(ONE - ax * ONE // L16, 0, ONE) * (ONE * 64 // (64 + ay * ay)) // ONE
        ry = np.clip(ONE - ay * ONE // L16, 0, ONE) * (ONE * 64 // (64 + ax * ax)) // ONE
        rays_light = (rx * rx // ONE + ry * ry // ONE) // 2
        light = (core * 2 + glow + rays_light) * b // ONE
        img[y0:y1, x0:x1] += light[..., None] * (np.array(rgb, I64) * 256)[None, None, :] // ONE


def vignette(img, cx, cy, rx, ry, strength):
    """Darkens towards the edges: 0 at the centre, `strength` (of ONE) at the ellipse (rx, ry) and beyond."""
    h, w = img.shape[:2]
    yy = (np.arange(h, dtype=I64)[:, None] - cy) * 1000 // ry
    xx = (np.arange(w, dtype=I64)[None, :] - cx) * 1000 // rx
    d = np.clip((xx * xx + yy * yy) // 1000, 0, 1000)
    mul(img, ONE - d * d // 1000 * strength // 1000)


def calm(img, x0, y0, x1, y1, feather, keep):
    """Dims a rounded rectangle (feathered by `feather` px) to `keep` of ONE: where the system draws over the picture."""
    h, w = img.shape[:2]
    yy = np.arange(h, dtype=I64)[:, None]
    xx = np.arange(w, dtype=I64)[None, :]
    dx = np.maximum(np.maximum(x0 - xx, xx - x1), 0)
    dy = np.maximum(np.maximum(y0 - yy, yy - y1), 0)
    d = isqrt(dx * dx + dy * dy)
    t = smooth(np.clip(ONE - d * ONE // feather, 0, ONE))
    mul(img, ONE - t * (ONE - keep) // ONE)


def tone(img):
    """Light to 8-bit: linear up to 200, then a soft shoulder that approaches 255 (bright cores turn white, never clip hard)."""
    knee, room = 200 * 256, 55 * 256
    v = np.maximum(img, 0)
    over = np.maximum(v - knee, 0)
    v = np.where(v > knee, knee + over * room // (room + over), v)
    return ((v + 128) >> 8).clip(0, 255).astype(np.uint8)


def rounded_outline(img, x0, y0, x1, y1, radius, width, rgb, k=ONE):
    """An anti-aliased rounded-rectangle outline (4 x 4 samples per pixel)."""
    h, w = img.shape[:2]
    acc = np.zeros((h, w), I64)
    for sy in range(4):
        for sx in range(4):
            yy = np.arange(h, dtype=I64)[:, None] * 8 + 2 * sy + 1
            xx = np.arange(w, dtype=I64)[None, :] * 8 + 2 * sx + 1
            cx = np.clip(xx, (x0 + radius) * 8, (x1 - radius) * 8)
            cy = np.clip(yy, (y0 + radius) * 8, (y1 - radius) * 8)
            d = isqrt((xx - cx) ** 2 + (yy - cy) ** 2)
            acc += ((d <= radius * 8) & (d > (radius - width) * 8)).astype(I64)
    add(img, acc * ONE // 16 * k // ONE, rgb)


def gear(img, cx, cy, r, rgb, k=ONE):
    """A small gear of our own drawing (8 teeth), anti-aliased."""
    h, w = img.shape[:2]
    acc = np.zeros((h, w), I64)
    for sy in range(4):
        for sx in range(4):
            yy = np.arange(h, dtype=I64)[:, None] * 8 + 2 * sy + 1 - cy * 8
            xx = np.arange(w, dtype=I64)[None, :] * 8 + 2 * sx + 1 - cx * 8
            d = isqrt(xx * xx + yy * yy)
            a = iatan2(yy, xx)
            tooth = (icos((a * 8) & 65535) > 1400)
            outer = np.where(tooth, r * 8, r * 8 * 78 // 100)
            hole = r * 8 * 36 // 100
            acc += ((d <= outer) & (d >= hole)).astype(I64)
    add(img, acc * ONE // 16 * k // ONE, rgb)


# ------------------------------------------------------------------------------------------------ the five pictures

TITLE = 'Super Mario Galaxy'  # the only words in the pictures (plain text; no subtitle)

# LiveArea geometry assumed for the a1 style (the 840x500 background; tune from a console screenshot): the start gate (startup.png,
# 280x158, with the system's Start button below it) is centred; the page is closed by peeling its top-right corner, and the system
# writes the title in the top-left corner. These areas are kept calm.
GATE = (262, 96, 578, 392)       # x0, y0, x1, y1: the gate picture and its button, with some room
PEEL = (700, 0, 840, 110)        # the corner the player peels


def centre_x(text, face, size, width, tracking=0):
    return (width - text_width(text, face, size, tracking)) // 2


def make_pic0(s):
    cam = Camera(960, 544, 0, 0, 4)
    img, X, Y = s.sky(cam)
    if s.style == 'spiral':
        s.piece(img, X, Y, 0, -300, 900)
    elif s.style == 'comet':
        s.piece(img, X, Y, 900, -560, 2400)
    else:
        s.piece(img, X, Y, 0, -250, 950)
    s.stars(img, cam, 101, 140, bright=7)
    vignette(img, 480, 260, 640, 420, 520)
    p = s.p
    # the title alone, a little larger and lower than when it had a line under it, so that it closes the picture
    size = 72
    x = centre_x(TITLE, 'display', size, 960, -10)
    draw_text(img, TITLE, 'display', size, x, 466, p['title'], tracking=-10, glow=(11, p['accent'], ONE * 2 // 3),
              shadow=(3, 4, ONE * 2 // 3), gradient=p['title_low'])
    return img


def make_bg(s):
    cam = Camera(840, 500, 0, 0, 4)
    img, X, Y = s.sky(cam)
    # the centre-piece sits right of the gate, wholly inside the page
    if s.style == 'spiral':
        s.piece(img, X, Y, 1000, 60, 700)
    elif s.style == 'comet':
        s.piece(img, X, Y, 1080, -200, 2300)
    else:
        s.piece(img, X, Y, 1000, 40, 640)
    s.stars(img, cam, 202, 140, bright=6)
    calm(img, *GATE, 70, ONE * 62 // 100)
    calm(img, *PEEL, 60, ONE * 75 // 100)
    vignette(img, 420, 230, 560, 380, 450)
    return img


def make_startup(s):
    cam = Camera(280, 158, 0, 0, 12)
    img, X, Y = s.sky(cam)
    if s.style == 'spiral':
        s.piece(img, X, Y, -1000, 0, 760)
    elif s.style == 'comet':
        s.piece(img, X, Y, -700, -200, 1500)
    else:
        s.piece(img, X, Y, -1000, 0, 820)
    s.stars(img, cam, 303, 150, bright=2, hero_scale=ONE // 2)
    p = s.p
    for i, (line, base) in enumerate((('Super Mario', 74), ('Galaxy', 104))):
        draw_text(img, line, 'display', 25, 140, base, p['title'], tracking=-8, glow=(5, p['accent'], ONE // 2), shadow=(2, 2, ONE // 2),
                  gradient=p['title_low'])
    vignette(img, 140, 79, 190, 110, 350)
    return img


def make_icon(s):
    cam = Camera(128, 128, 0, 0, 16)
    img, X, Y = s.sky(cam)
    if s.style == 'spiral':
        s.piece(img, X, Y, 0, 0, 1240)  # the arms end about 54 px from the centre: inside the bubble with a margin
    elif s.style == 'comet':
        s.piece(img, X, Y, 260, -120, 1150)
    else:
        s.orbit(img, X, Y, 0, 0, 1150, rings=True)
    s.stars(img, cam, 404, 260, bright=1, hero_scale=ONE // 2)  # few: at 128 px a dense field reads as noise
    vignette(img, 64, 64, 66, 66, 560)   # the bubble's round mask: darker towards its rim, the centre-piece stays inside 52 px
    return img


def make_settings(s):
    cam = Camera(280, 64, 1600, 900, 14)
    img, X, Y = s.sky(cam)
    s.stars(img, cam, 505, 160, bright=0)
    p = s.p
    mul(img, np.full((64, 280), ONE * 85 // 100, I64))
    size = 26
    tw = text_width('Settings', 'display', size)
    x = (280 - tw + 34) // 2
    gear(img, x - 22, 32, 11, p['edge'], ONE * 9 // 10)
    draw_text(img, 'Settings', 'display', size, x, 41, p['title'], glow=(4, p['accent'], ONE // 2), shadow=(2, 2, ONE // 2))
    rounded_outline(img, 1, 1, 279, 63, 10, 2, p['edge'], ONE * 9 // 10)  # the frame the template's tile has always had
    return img


PICTURES = (('icon0.png', make_icon), ('pic0.png', make_pic0), ('livearea/contents/bg.png', make_bg),
            ('livearea/contents/startup.png', make_startup), ('livearea/contents/settings.png', make_settings))


def render(style, name):
    return tone(dict(PICTURES)[name](Scene(style)))


def save(rgb, path):
    idx, pal = palette_png.quantize(rgb)
    data = palette_png.png8(idx, pal)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return len(data)


def main(argv):
    style, out = DEFAULT_STYLE, os.path.join(HERE, '..', '..', 'vita', 'vgx-game', 'sce_sys')
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == '--style':
            style = args.pop(0)
        elif a.startswith('--style='):
            style = a.split('=', 1)[1]
        else:
            out = a
    if style not in STYLES:
        sys.exit(f'unknown style {style!r}: one of {", ".join(STYLES)}')
    for name, _ in PICTURES:
        rgb = render(style, name)
        n = save(rgb, os.path.join(out, name))
        print(name, f'{rgb.shape[1]}x{rgb.shape[0]}', n, 'bytes')


if __name__ == '__main__':
    main(sys.argv[1:])

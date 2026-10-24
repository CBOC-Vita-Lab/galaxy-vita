#!/usr/bin/env python3
"""Offline pictures of the Vita port's settings panel (platform/src/port/settings_panel*.cpp) at the console's 960 x 544.

    render.py <cooked data root> <output dir>
        e.g. render.py cooked build/settings-panel-shots

The panel's plan comes from the same C++ the console runs (preview.cpp + settings_panel_plan.cpp, built here with the host's
clang++); the words use the game's message font and the buttons the picture font with the handheld patch's button pictures
(platform/src/port/handheld_glyphs.inc), read from the owner's local data and drawn here as the GX path would draw them
(coloured triangles; glyph quads sampled bilinearly, I4: alpha = intensity, RGB5A3: modulated). Behind it, a schematic of the
pause menu: the real rectangles of its buttons, bands, counters and Luigi letter button from LayoutData/PauseMenu.arc, as plain
shapes (not the game's pictures). The output is local only: it contains the game's glyphs.
"""
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SW, SH = 960, 544


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


# ---------------------------------------------------------------- cooked (little-endian) RARC and RFNT
def rarc_files(b):
    hdr = u32(b, 8)
    data = hdr + u32(b, 0xC)
    ndirs, dir_off, nfiles, file_off, _, str_off = (u32(b, hdr + 4 * i) for i in range(6))
    dir_off += hdr; file_off += hdr; str_off += hdr
    def cstr(o):
        e = b.index(b"\0", str_off + o)
        return b[str_off + o:e].decode("shift_jis", "replace")
    dirs = [(cstr(u32(b, dir_off + 16 * i + 4)), u16(b, dir_off + 16 * i + 10), u32(b, dir_off + 16 * i + 12)) for i in range(ndirs)]
    out = {}
    def walk(di, prefix):
        _, count, first = dirs[di]
        for e in range(first, first + count):
            f = file_off + 0x14 * e
            w = u32(b, f + 4)
            name = cstr(w & 0xFFFFFF)
            if (w >> 24) & 2:
                if name not in (".", ".."): walk(u32(b, f + 8), prefix + name + "/")
            else:
                out[(prefix + name).lower()] = b[data + u32(b, f + 8):data + u32(b, f + 8) + u32(b, f + 12)]
    walk(0, dirs[0][0] + "/")
    return out


class RFont:
    def __init__(self, f):
        self.f = f
        self.cmap, self.widths = {}, {}
        o = u16(f, 0xC)
        for _ in range(u16(f, 0xE)):
            kind, size, d = f[o:o + 4][::-1], u32(f, o + 4), o + 8
            if kind == b"FINF":
                self.alt = u16(f, d + 2)
                self.height, self.width, self.ascent = f[d + 0x14], f[d + 0x15], f[d + 0x16]
            elif kind == b"TGLP":
                self.cw, self.ch, self.base = f[d], f[d + 1], f[d + 2]
                self.sheet_size, self.fmt = u32(f, d + 4), u16(f, d + 10)
                self.row, self.line, self.sw, self.sh = u16(f, d + 12), u16(f, d + 14), u16(f, d + 16), u16(f, d + 18)
                self.img = u32(f, d + 0x14)
            elif kind == b"CWDH":
                a, z = u16(f, d), u16(f, d + 2)
                for k in range(a, z + 1):
                    self.widths[k] = struct.unpack_from("<bBB", f, d + 8 + 3 * (k - a))
            elif kind == b"CMAP":
                a, z, m = u16(f, d), u16(f, d + 2), u16(f, d + 4)
                if m == 0:
                    for c in range(a, z + 1): self.cmap[c] = u16(f, d + 0xC) + c - a
                elif m == 1:
                    for c in range(a, z + 1):
                        g = u16(f, d + 0xC + 2 * (c - a))
                        if g != 0xFFFF: self.cmap[c] = g
                else:
                    for k in range(u16(f, d + 0xC)): self.cmap[u16(f, d + 0xE + 4 * k)] = u16(f, d + 0x10 + 4 * k)
            o += size
        self.sheets = {}

    def sheet(self, k):
        if k in self.sheets: return self.sheets[k]
        w, h, base, d = self.sw, self.sh, self.img + k * self.sheet_size, self.f
        px = np.zeros((h, w, 4), np.float32)
        if self.fmt == 0:  # I4, 8 x 8 tiles
            raw = np.frombuffer(d, np.uint8, self.sheet_size, base)
            nib = np.stack([raw >> 4, raw & 15], -1).reshape(h // 8, w // 8, 8, 8)
            img = nib.transpose(0, 2, 1, 3).reshape(h, w).astype(np.float32) / 15.0
            px[..., 0] = px[..., 1] = px[..., 2] = 1.0
            px[..., 3] = img
        elif self.fmt == 5:  # RGB5A3, 4 x 4 tiles, big-endian texels
            raw = np.frombuffer(d, ">u2", self.sheet_size // 2, base).astype(np.int32).reshape(h // 4, w // 4, 4, 4)
            v = raw.transpose(0, 2, 1, 3).reshape(h, w)
            op = (v & 0x8000) != 0
            px[..., 0] = np.where(op, ((v >> 10) & 31) / 31.0, ((v >> 8) & 15) / 15.0)
            px[..., 1] = np.where(op, ((v >> 5) & 31) / 31.0, ((v >> 4) & 15) / 15.0)
            px[..., 2] = np.where(op, (v & 31) / 31.0, (v & 15) / 15.0)
            px[..., 3] = np.where(op, 1.0, ((v >> 12) & 7) / 7.0)
        else:
            raise SystemExit("font sheet format %d not handled" % self.fmt)
        self.sheets[k] = px
        return px

    def glyph(self, code):
        """(picture HxWx4 of the cell's glyphWidth x cellHeight, left, glyphWidth, charWidth)"""
        g = self.cmap.get(code, self.alt)
        left, gw, cw = self.widths.get(g, (0, self.cw, self.cw))
        per = self.row * self.line
        sheet = self.sheet(g // per)
        c = g % per
        x0, y0 = (c % self.row) * (self.cw + 1) + 1, (c // self.row) * (self.ch + 1) + 1
        return g, sheet[y0:y0 + self.ch, x0:x0 + gw], left, gw, cw


def handheld_glyphs():
    """The handheld patch's button pictures (our own drawings, generated by tools/handheld/make_glyphs.py)."""
    text = open(os.path.join(ROOT, "platform", "src", "port", "handheld_glyphs.inc")).read()
    out = {}
    import re
    for m in re.finditer(r"kGlyph(\w+)\[(\d+)\] = \{([^}]*)\}", text):
        vals = np.array([int(x) for x in m.group(3).replace("\n", "").split(",") if x.strip()], np.float32) / 255.0
        out[m.group(1)] = vals.reshape(35, 34, 4)
    # the picture font's glyph indices they replace (handheld_ui.cpp kCells)
    return {1: out["Cross"], 2: out["Circle"], 3: out["Triangle"], 6: out["Square"], 7: out["R"], 24: out["Select"], 25: out["Start"], 26: out["L"]}


# ---------------------------------------------------------------- drawing at the console's resolution
class Canvas:
    def __init__(self, W, H):
        self.W, self.H = W, H  # square units of the picture
        self.sx, self.sy = SW / W, SH / H
        self.px = np.zeros((SH, SW, 3), np.float32)

    def blend(self, x0, y0, rgb, a):
        h, w = a.shape
        dst = self.px[y0:y0 + h, x0:x0 + w]
        dst[:] = dst * (1 - a[..., None]) + rgb * a[..., None]

    def tri(self, v):
        xs = np.array([p[0] * self.sx for p in v]); ys = np.array([p[1] * self.sy for p in v])
        cols = np.array([p[2] for p in v], np.float32) / 255.0
        x0, x1 = max(int(np.floor(xs.min())), 0), min(int(np.ceil(xs.max())), SW)
        y0, y1 = max(int(np.floor(ys.min())), 0), min(int(np.ceil(ys.max())), SH)
        if x1 <= x0 or y1 <= y0: return
        X, Y = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(d) < 1e-9: return
        l0 = ((ys[1] - ys[2]) * (X - xs[2]) + (xs[2] - xs[1]) * (Y - ys[2])) / d
        l1 = ((ys[2] - ys[0]) * (X - xs[2]) + (xs[0] - xs[2]) * (Y - ys[2])) / d
        l2 = 1 - l0 - l1
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any(): return
        c = l0[..., None] * cols[0] + l1[..., None] * cols[1] + l2[..., None] * cols[2]
        self.blend(x0, y0, c[..., :3], np.where(inside, c[..., 3], 0.0))

    def quad_tex(self, left, top, right, bottom, tex, rgba):
        """A glyph quad (square units), the texture stretched over it, bilinear, modulated by rgba (0..1)."""
        X0, X1, Y0, Y1 = left * self.sx, right * self.sx, top * self.sy, bottom * self.sy
        x0, x1 = max(int(np.floor(X0)), 0), min(int(np.ceil(X1)), SW)
        y0, y1 = max(int(np.floor(Y0)), 0), min(int(np.ceil(Y1)), SH)
        if x1 <= x0 or y1 <= y0: return
        th, tw = tex.shape[:2]
        X, Y = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        u = (X - X0) / (X1 - X0) * tw - 0.5
        v = (Y - Y0) / (Y1 - Y0) * th - 0.5
        inside = (X >= X0) & (X < X1) & (Y >= Y0) & (Y < Y1)
        iu, iv = np.floor(u).astype(int), np.floor(v).astype(int)
        fu, fv = (u - iu)[..., None], (v - iv)[..., None]
        def at(a, b): return tex[np.clip(b, 0, th - 1), np.clip(a, 0, tw - 1)]
        s = (at(iu, iv) * (1 - fu) * (1 - fv) + at(iu + 1, iv) * fu * (1 - fv) + at(iu, iv + 1) * (1 - fu) * fv + at(iu + 1, iv + 1) * fu * fv)
        col = s[..., :3] * np.array(rgba[:3], np.float32)
        self.blend(x0, y0, col, np.where(inside, s[..., 3] * rgba[3], 0.0))

    def run(self, font, x, y, sx, sy, rgba, codes, patched=None, is_picture=False):
        rgba = [c / 255.0 for c in rgba]
        for code in codes:
            g, tex, left, gw, cw = font.glyph(code)
            if is_picture and patched and g in patched:
                tex = patched[g][:, :gw]
            if not is_picture:  # I4: the colour is the vertex colour, the texture only gives alpha
                t = np.ones_like(tex); t[..., 3] = tex[..., 3]; tex = t
            if gw > 0:
                self.quad_tex(x + left * sx, y, x + (left + gw) * sx, y + font.ch * sy, tex, rgba)
            x += cw * sx

    def text(self, font, s, x, cy, size, rgba, align=1):
        sc = size / font.height
        w = sum(font.glyph(ord(c))[4] for c in s) * sc
        left = x - w * (0.5 if align == 1 else (1.0 if align == 2 else 0.0))
        self.run(font, left + size * 0.07, cy - 17.4 * sc + size * 0.1, sc, sc, (6, 8, 40, int(170 * rgba[3] / 255)), [ord(c) for c in s])
        self.run(font, left, cy - 17.4 * sc, sc, sc, rgba, [ord(c) for c in s])

    def image(self):
        return Image.fromarray((np.clip(self.px, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def rrect(cv, x0, y0, x1, y1, r, top, bottom, seg=6):
    """a filled rounded rectangle (square units) for the backdrop, as triangles"""
    import math
    pts = []
    for k, (cx, cy) in enumerate([(x0 + r, y0 + r), (x1 - r, y0 + r), (x1 - r, y1 - r), (x0 + r, y1 - r)]):
        for s in range(seg + 1):
            a = math.pi * (1 + 0.5 * k) + 0.5 * math.pi * s / seg
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    def col(y):
        t = min(max((y - y0) / max(y1 - y0, 1e-6), 0), 1)
        return tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
    c = ((x0 + x1) / 2, (y0 + y1) / 2)
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        cv.tri([(c[0], c[1], col(c[1])), (a[0], a[1], col(a[1])), (b[0], b[1], col(b[1]))])


def backdrop(cv, msg, letter):
    """The pause menu's rectangles (LayoutData/PauseMenu.arc, see settings_panel_plan.cpp), as plain shapes."""
    W, H = cv.W, cv.H
    mx, my = W / 2, H / 2
    L = lambda lx: mx + lx * 4.0 / 3.0   # layout x -> square units (16:9)
    Y = lambda ly: my - ly
    # a dim, darkened "game" behind the menu
    yy = np.linspace(0, 1, SH)[:, None, None]
    cv.px[:] = (np.array([0.10, 0.16, 0.30]) * (1 - yy) + np.array([0.05, 0.07, 0.12]) * yy)
    # the bands at the top (stage name) and the bottom (counters)
    rrect(cv, -10, Y(157 + 80), W + 10, Y(157 - 80), 0, (0, 0, 0, 120), (0, 0, 0, 40))
    rrect(cv, -10, Y(-183 + 60), W + 10, Y(-183 - 60), 0, (0, 0, 0, 60), (0, 0, 0, 140))
    cv.text(msg, "Gateway Galaxy", mx, Y(157 + 18 - 2) + 4, 27, (255, 255, 255, 255))
    # the two buttons: 341 x 77 square units, a shadow 6 lower
    for ly in (24, -73):
        rrect(cv, mx - 170.7, Y(ly - 6) - 38.7, mx + 170.7, Y(ly - 6) + 38.7, 38.7, (0, 0, 30, 64), (0, 0, 30, 64))
        rrect(cv, mx - 170.7, Y(ly) - 38.7, mx + 170.7, Y(ly) + 38.7, 38.7, (250, 250, 255, 235), (205, 215, 240, 235))
        cv.text(msg, "(pause menu button)", mx, Y(ly + 2), 24, (40, 50, 110, 255))
    # the counters (their whole text boxes): star / lives at the left, coins / Star Bits at the right (four-figure Star Bits)
    for x0, x1, ly in ((L(-270), L(-270) + 129, -143), (L(-270), L(-270) + 104, -179), (628, 800, -143), (628, 800, -181)):
        rrect(cv, x0, Y(ly) - 16, x1, Y(ly) + 16, 6, (255, 255, 255, 46), (255, 255, 255, 46))
        cv.text(msg, "counter", (x0 + x1) / 2, Y(ly), 16, (255, 255, 255, 150))
    if letter:  # the Luigi letter button, 183 x 70 at y -213
        rrect(cv, mx - 91.5, Y(-213) - 35.2, mx + 91.5, Y(-213) + 35.2, 35.2, (250, 250, 255, 235), (205, 215, 240, 235))
        cv.text(msg, "(letter)", mx, Y(-213) - 12, 20, (40, 50, 110, 255))


def main():
    data, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "preview-work")
    os.makedirs(work, exist_ok=True)
    arc = rarc_files(open(os.path.join(data, "files", "LayoutData", "Font.arc"), "rb").read())
    msg = RFont(arc["font/messagefont26.brfnt"])
    pic = RFont(arc["font/picturefont.brfnt"])
    metrics = os.path.join(work, "metrics.txt")
    with open(metrics, "w") as f:
        for i, font in enumerate((msg, pic)):
            f.write("H %d %d %d\n" % (i, font.height, font.base))
            for code, g in font.cmap.items():
                f.write("C %d %d %d\n" % (i, code, font.widths.get(g, (0, 0, font.cw))[2]))
    exe = os.path.join(work, "preview")
    srcs = [os.path.join(HERE, "preview.cpp"), os.path.join(ROOT, "platform", "src", "port", "settings_panel_plan.cpp")]
    subprocess.check_call(["clang++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-o", exe] + srcs)
    patched = handheld_glyphs()
    scenes = [
        ("panel-en-off", 15, 0, 0.0, 0.0, 0, "UsEnglish"),
        ("panel-en-on", 17, 1, 1.0, 0.0, 0, "UsEnglish"),
        ("panel-en-switching", 10, 1, 0.5, 1.0, 0, "UsEnglish"),
        ("panel-en-max", 20, 1, 1.0, 0.0, 0, "UsEnglish"),
        ("panel-en-letter", 15, 0, 0.0, 0.0, 1, "UsEnglish"),
        ("panel-fr", 15, 1, 1.0, 0.0, 0, "UsFrench"),
        ("panel-es", 15, 0, 0.0, 0.0, 0, "UsSpanish"),
        ("panel-de", 13, 1, 1.0, 0.0, 0, "EuGerman"),
        ("panel-it", 15, 0, 0.0, 0.0, 0, "EuItalian"),
        ("panel-nl", 15, 1, 1.0, 0.0, 0, "EuDutch"),
        ("panel-ja", 15, 1, 1.0, 0.0, 0, "JpJapanese"),
    ]
    for name, zoom, rear, knob, pulse, compact, lang in scenes:
        txt = subprocess.check_output([exe, metrics, str(zoom), str(rear), str(knob), str(pulse), "1", str(compact), lang]).decode()
        lines = txt.splitlines()
        head = lines[0].split()
        W, H = float(head[1]), float(head[2])
        cv = Canvas(W, H)
        backdrop(cv, msg, compact)
        verts = []
        for l in lines[1:]:
            p = l.split()
            if p[0] == "V":
                verts.append((float(p[1]), float(p[2]), tuple(int(c) for c in p[3:7])))
                if len(verts) == 3:
                    cv.tri(verts)
                    verts = []
        for l in lines[1:]:  # (words after shapes, message font first, then the buttons: as settings_panel.cpp draws them)
            p = l.split()
            if p[0] == "R" and p[1] == "0":
                cv.run(msg, float(p[2]), float(p[3]), float(p[4]), float(p[5]), [int(c) for c in p[6:10]], [int(c) for c in p[10:]])
        for l in lines[1:]:
            p = l.split()
            if p[0] == "R" and p[1] == "1":
                cv.run(pic, float(p[2]), float(p[3]), float(p[4]), float(p[5]), [int(c) for c in p[6:10]], [int(c) for c in p[10:]],
                       patched, True)
        path = os.path.join(out, name + ".png")
        cv.image().save(path)
        print("%s: panel %s-%s x %s-%s (square units), %s vertices, overflow %s, language %s" % (path, head[3], head[5], head[4], head[6], head[7], head[8], head[9]))


if __name__ == "__main__":
    main()

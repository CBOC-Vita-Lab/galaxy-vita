#!/usr/bin/env python3
"""A small offline renderer for the game's own 2D layouts (cooked, little-endian BRLYT + TPL from LayoutData/*.arc), enough to
preview the multiplayer menus with the real buttons, windows, title bands and head icons.

What it does: pane tree (pan1/pic1/txt1/wnd1/bnd1), translate / scale / size / base position / alpha (influenced alpha), the 16:9
location adjust (x scale 0.75 for flagged panes, as LayoutManager::calcAnim sets it), materials with their TEV stages (or nw4r's
default stages when a material has none: lerp(C0, C1, texture) x rasterised colour), textures (I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8
CMPR), picture texture coordinates and wrap modes, window panes (content + 1/2/4 frame materials, nw4r's corner/edge layout), and
text boxes in the game's fonts. It does not run animations (the static layout is the "wait" pose), indirect textures or blend
modes other than alpha blending. Local data only: the output contains the game's art.
"""
import math
import struct

import numpy as np

SW, SH = 960, 544


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def s16(b, o): return struct.unpack_from("<h", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def f32(b, o): return struct.unpack_from("<f", b, o)[0]


# ------------------------------------------------------------------ RARC (cooked)
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


# ------------------------------------------------------------------ TPL
def _tiles(raw, w, h, bw, bh):
    """raw (blocks of bh x bw texels, row-major blocks) -> h x w"""
    pw, ph = (w + bw - 1) // bw * bw, (h + bh - 1) // bh * bh
    t = raw[:pw * ph].reshape(ph // bh, pw // bw, bh, bw)
    return t.transpose(0, 2, 1, 3).reshape(ph, pw)[:h, :w]


def decode_tpl(b):
    """the first image of a TPL as float32 h x w x 4 (rgba 0..1)"""
    n = u32(b, 4); tab = u32(b, 8)
    ih = u32(b, tab)
    h, w, fmt, off = u16(b, ih), u16(b, ih + 2), u32(b, ih + 4), u32(b, ih + 8)
    px = np.zeros((h, w, 4), np.float32)
    if fmt == 0:  # I4
        pw, ph = (w + 7) // 8 * 8, (h + 7) // 8 * 8
        raw = np.frombuffer(b, np.uint8, pw * ph // 2, off)
        nib = np.stack([raw >> 4, raw & 15], -1).reshape(-1)
        i = _tiles(nib, w, h, 8, 8).astype(np.float32) / 15
        px[..., 0] = px[..., 1] = px[..., 2] = px[..., 3] = i
    elif fmt == 1:  # I8
        pw, ph = (w + 7) // 8 * 8, (h + 3) // 4 * 4
        i = _tiles(np.frombuffer(b, np.uint8, pw * ph, off), w, h, 8, 4).astype(np.float32) / 255
        px[..., 0] = px[..., 1] = px[..., 2] = px[..., 3] = i
    elif fmt == 2:  # IA4
        pw, ph = (w + 7) // 8 * 8, (h + 3) // 4 * 4
        v = _tiles(np.frombuffer(b, np.uint8, pw * ph, off), w, h, 8, 4)
        i = (v & 15).astype(np.float32) / 15; a = (v >> 4).astype(np.float32) / 15
        px[..., 0] = px[..., 1] = px[..., 2] = i; px[..., 3] = a
    elif fmt == 3:  # IA8 (alpha, intensity bytes)
        pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        raw = np.frombuffer(b, np.uint8, pw * ph * 2, off).reshape(-1, 2)
        a = _tiles(raw[:, 0].copy(), w, h, 4, 4).astype(np.float32) / 255
        i = _tiles(raw[:, 1].copy(), w, h, 4, 4).astype(np.float32) / 255
        px[..., 0] = px[..., 1] = px[..., 2] = i; px[..., 3] = a
    elif fmt in (4, 5):  # RGB565 / RGB5A3, big-endian texels
        pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        v = _tiles(np.frombuffer(b, ">u2", pw * ph, off).astype(np.int32), w, h, 4, 4)
        if fmt == 4:
            px[..., 0] = ((v >> 11) & 31) / 31; px[..., 1] = ((v >> 5) & 63) / 63; px[..., 2] = (v & 31) / 31; px[..., 3] = 1
        else:
            op = (v & 0x8000) != 0
            px[..., 0] = np.where(op, ((v >> 10) & 31) / 31, ((v >> 8) & 15) / 15)
            px[..., 1] = np.where(op, ((v >> 5) & 31) / 31, ((v >> 4) & 15) / 15)
            px[..., 2] = np.where(op, (v & 31) / 31, (v & 15) / 15)
            px[..., 3] = np.where(op, 1.0, ((v >> 12) & 7) / 7)
    elif fmt == 6:  # RGBA8: per 4x4 block, 32 bytes AR then 32 bytes GB
        pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        raw = np.frombuffer(b, np.uint8, pw * ph * 4, off).reshape(-1, 64)
        ar = raw[:, :32].reshape(-1, 16, 2); gb = raw[:, 32:].reshape(-1, 16, 2)
        for k, src in enumerate((ar[..., 1], gb[..., 0], gb[..., 1], ar[..., 0])):
            px[..., k] = _tiles(src.reshape(-1).copy(), w, h, 4, 4).astype(np.float32) / 255
    elif fmt == 14:  # CMPR: 8x8 tiles of four 4x4 DXT1 blocks
        pw, ph = (w + 7) // 8 * 8, (h + 7) // 8 * 8
        out = np.zeros((ph, pw, 4), np.float32)
        o = off
        for ty in range(0, ph, 8):
            for tx in range(0, pw, 8):
                for sb in range(4):
                    c0, c1 = struct.unpack_from(">HH", b, o)
                    bits = struct.unpack_from(">I", b, o + 4)[0]
                    o += 8

                    def rgb(c): return np.array([((c >> 11) & 31) / 31, ((c >> 5) & 63) / 63, (c & 31) / 31, 1.0], np.float32)
                    p0, p1 = rgb(c0), rgb(c1)
                    if c0 > c1:
                        pal = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
                    else:
                        pal = [p0, p1, (p0 + p1) / 2, np.zeros(4, np.float32)]
                    bx, by = tx + (sb & 1) * 4, ty + (sb >> 1) * 4
                    for k in range(16):
                        out[by + k // 4, bx + k % 4] = pal[(bits >> (30 - 2 * k)) & 3]
        px = out[:h, :w].copy()
    else:
        px[..., :] = 1.0  # (palette formats: not used by the layouts previewed)
    return px


# ------------------------------------------------------------------ fonts (RFNT, cooked)
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
        self.patched = None

    def sheet(self, k):
        if k in self.sheets: return self.sheets[k]
        w, h, base, d = self.sw, self.sh, self.img + k * self.sheet_size, self.f
        px = np.zeros((h, w, 4), np.float32)
        if self.fmt == 0:
            raw = np.frombuffer(d, np.uint8, self.sheet_size, base)
            nib = np.stack([raw >> 4, raw & 15], -1).reshape(h // 8, w // 8, 8, 8)
            img = nib.transpose(0, 2, 1, 3).reshape(h, w).astype(np.float32) / 15.0
            px[..., 0] = px[..., 1] = px[..., 2] = 1.0
            px[..., 3] = img
        elif self.fmt == 5:
            raw = np.frombuffer(d, ">u2", self.sheet_size // 2, base).astype(np.int32).reshape(h // 4, w // 4, 4, 4)
            v = raw.transpose(0, 2, 1, 3).reshape(h, w)
            op = (v & 0x8000) != 0
            px[..., 0] = np.where(op, ((v >> 10) & 31) / 31.0, ((v >> 8) & 15) / 15.0)
            px[..., 1] = np.where(op, ((v >> 5) & 31) / 31.0, ((v >> 4) & 15) / 15.0)
            px[..., 2] = np.where(op, (v & 31) / 31.0, (v & 15) / 15.0)
            px[..., 3] = np.where(op, 1.0, ((v >> 12) & 7) / 7.0)
        elif self.fmt == 2:  # IA4
            raw = np.frombuffer(d, np.uint8, self.sheet_size, base).reshape(h // 4, w // 8, 4, 8)
            v = raw.transpose(0, 2, 1, 3).reshape(h, w)
            px[..., 0] = px[..., 1] = px[..., 2] = (v & 15) / 15.0
            px[..., 3] = (v >> 4) / 15.0
        else:
            raise SystemExit("font sheet format %d not handled" % self.fmt)
        self.sheets[k] = px
        return px

    def glyph(self, code):
        g = self.cmap.get(code, self.alt)
        left, gw, cw = self.widths.get(g, (0, self.cw, self.cw))
        per = self.row * self.line
        sheet = self.sheet(g // per)
        c = g % per
        x0, y0 = (c % self.row) * (self.cw + 1) + 1, (c // self.row) * (self.ch + 1) + 1
        tex = sheet[y0:y0 + self.ch, x0:x0 + gw]
        if self.patched and g in self.patched:
            tex = self.patched[g][:, :gw]
        return g, tex, left, gw, cw

    def advance(self, code):
        g = self.cmap.get(code, self.alt)
        return self.widths.get(g, (0, self.cw, self.cw))[2]


# ------------------------------------------------------------------ canvas
class Canvas:
    """960 x 544 pixels. Coordinates given in 'square units' (y down, origin top left): W x H."""
    def __init__(self, W=810.6667, H=456.0):
        self.W, self.H = W, H
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

    def shade_quad(self, corners, shader):
        """an axis-aligned quad in pixels (x0, y0, x1, y1); shader(u, v) -> rgba (h x w x 4), u, v in 0..1 across the quad"""
        X0, Y0, X1, Y1 = corners
        if X1 < X0: X0, X1 = X1, X0
        if Y1 < Y0: Y0, Y1 = Y1, Y0
        x0, x1 = max(int(np.floor(X0)), 0), min(int(np.ceil(X1)), SW)
        y0, y1 = max(int(np.floor(Y0)), 0), min(int(np.ceil(Y1)), SH)
        if x1 <= x0 or y1 <= y0 or X1 - X0 < 1e-6 or Y1 - Y0 < 1e-6: return
        X, Y = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        u = (X - X0) / (X1 - X0); v = (Y - Y0) / (Y1 - Y0)
        cov = ((X >= X0) & (X < X1) & (Y >= Y0) & (Y < Y1)).astype(np.float32)
        rgba = shader(np.clip(u, 0, 1), np.clip(v, 0, 1))
        self.blend(x0, y0, rgba[..., :3], np.clip(rgba[..., 3], 0, 1) * cov)

    def image(self):
        from PIL import Image
        return Image.fromarray((np.clip(self.px, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def sample(tex, u, v, wrapS=0, wrapT=0):
    """bilinear; u, v in texture space 0..1 (may exceed with repeat)"""
    th, tw = tex.shape[:2]

    def wrap(c, n, mode):
        if mode == 1: return np.mod(c, n)
        if mode == 2:
            m = np.mod(c, 2 * n)
            return np.where(m >= n, 2 * n - 1 - m, m)
        return np.clip(c, 0, n - 1)
    x = u * tw - 0.5; y = v * th - 0.5
    ix, iy = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - ix)[..., None], (y - iy)[..., None]

    def at(a, b): return tex[wrap(b, th, wrapT), wrap(a, tw, wrapS)]
    return at(ix, iy) * (1 - fx) * (1 - fy) + at(ix + 1, iy) * fx * (1 - fy) + at(ix, iy + 1) * (1 - fx) * fy + at(ix + 1, iy + 1) * fx * fy


# ------------------------------------------------------------------ materials and TEV
KCONST = [1.0, 7 / 8, 6 / 8, 5 / 8, 4 / 8, 3 / 8, 2 / 8, 1 / 8]


class Material:
    def __init__(self, b, o, textures):
        self.name = b[o:o + 20].split(b"\0")[0].decode("latin1")
        self.tevc = [np.array([s16(b, o + 20 + 8 * i + 2 * k) for k in range(4)], np.float32) / 255 for i in range(3)]
        self.kc = [np.array(list(b[o + 44 + 4 * i:o + 48 + 4 * i]), np.float32) / 255 for i in range(4)]
        bits = u32(b, o + 60)
        q = o + 64
        ntex, nsrt, ngen = bits & 15, (bits >> 4) & 15, (bits >> 8) & 15
        self.texmaps = []
        for i in range(ntex):
            idx, ws, wt = u16(b, q), b[q + 2], b[q + 3]
            self.texmaps.append((textures[idx] if idx < len(textures) else None, ws & 3, wt & 3))
            q += 4
        self.srts = []
        for i in range(nsrt):
            self.srts.append(struct.unpack_from("<5f", b, q)); q += 20
        q += 4 * ngen
        self.swaps = None  # the 4 TEV swap tables ((r, g, b, a) source channels each), for materials given their own stages by the caller
        self.swap0 = None  # TEV swap table 0 (r, g, b, a source channels), set by the caller; None = identity
        self.chan_src_reg = None
        if (bits >> 25) & 1:
            self.chan_src_reg = b[q] == 0  # GX_SRC_REG = 0
            q += 4
        self.matcol = None
        if (bits >> 27) & 1:
            self.matcol = np.array(list(b[q:q + 4]), np.float32) / 255; q += 4
        if (bits >> 12) & 1: q += 4  # (4 swap modes, a byte each)
        q += 20 * ((bits >> 13) & 3)
        q += 4 * ((bits >> 15) & 7)
        self.stages = []
        for i in range((bits >> 18) & 31):
            st = b[q:q + 16]
            self.stages.append(st); q += 16

    def kcolor(self, sel, alpha=False):
        if sel < 8: return np.array([KCONST[sel]] * 4, np.float32)
        if sel >= 0x0C and sel <= 0x0F and not alpha: return self.kc[sel - 0x0C]
        comp = (sel - 0x10) // 4; reg = (sel - 0x10) % 4
        if sel >= 0x10: return np.array([self.kc[reg][comp]] * 4, np.float32)
        return np.ones(4, np.float32)

    def shade(self, ras, texcoords):
        """ras: h x w x 4 (rasterised colour); texcoords: list of (u, v) arrays per tex coord. Returns h x w x 4."""
        h, w = ras.shape[:2]
        texs = []
        for i, (tex, ws, wt) in enumerate(self.texmaps):
            if tex is None or not texcoords:
                texs.append(np.ones((h, w, 4), np.float32)); continue
            u, v = texcoords[i] if i < len(texcoords) else texcoords[0]   # (a second texture on the first coordinates: the star pointer's hand)
            if i < len(self.srts):
                tx, ty, rot, sx, sy = self.srts[i]
                u = (u - 0.5) * sx + 0.5 + tx; v = (v - 0.5) * sy + 0.5 + ty
            texs.append(sample(tex, u, v, ws, wt))
        if self.swap0 and not self.stages:
            texs = [t[..., list(self.swap0)] for t in texs]
        regs = [np.broadcast_to(c, (h, w, 4)).astype(np.float32) for c in self.tevc]
        prev = np.zeros((h, w, 4), np.float32)
        stages = self.stages
        if not stages:
            # nw4r's stages for a material without its own (lyt_material.cpp SetupGX)
            if not texs:
                out = regs[1] * ras
                return out
            if len(texs) == 1:
                t = texs[0]
            else:
                t = texs[0]
                for k in range(1, len(texs)):
                    kk = self.kcolor([0x1F, 0x1B, 0x17, 0x13][k % 4] if k < 4 else 0x1F)
                    t = t * kk + texs[k] * (1 - kk)
            out = regs[0] + (regs[1] - regs[0]) * t
            return np.clip(out, 0, 1) * ras
        for st in stages:
            texcoord, chan, texmap, swap = st[0], st[1], st[2], st[3]
            tex = texs[texmap] if texmap < len(texs) else np.ones((h, w, 4), np.float32)
            if self.swaps: tex = tex[..., list(self.swaps[(swap >> 3) & 3])]   # (the stage's texture swap table)
            ab, cd, op, cl = st[4], st[5], st[6], st[7]
            aab, acd, aop, acl = st[8], st[9], st[10], st[11]
            kc = self.kcolor((cl >> 3) & 31); ka = self.kcolor((acl >> 3) & 31, True)[3]

            def carg(i):
                return [prev[..., :3], prev[..., 3:4].repeat(3, -1), regs[0][..., :3], regs[0][..., 3:4].repeat(3, -1),
                        regs[1][..., :3], regs[1][..., 3:4].repeat(3, -1), regs[2][..., :3], regs[2][..., 3:4].repeat(3, -1),
                        tex[..., :3], tex[..., 3:4].repeat(3, -1), ras[..., :3], ras[..., 3:4].repeat(3, -1),
                        np.ones((h, w, 3), np.float32), np.full((h, w, 3), 0.5, np.float32),
                        np.broadcast_to(kc[:3], (h, w, 3)), np.zeros((h, w, 3), np.float32)][i]

            def aarg(i):
                return [prev[..., 3], regs[0][..., 3], regs[1][..., 3], regs[2][..., 3], tex[..., 3], ras[..., 3],
                        np.full((h, w), ka, np.float32), np.zeros((h, w), np.float32)][i]

            def comb(a, b_, c, d, o, clamp):
                op_, bias, scale = o & 15, (o >> 4) & 3, (o >> 6) & 3
                r = a * (1 - c) + b_ * c
                r = d - r if op_ == 1 else d + r
                r = r + (0.5 if bias == 1 else (-0.5 if bias == 2 else 0))
                r = r * [1, 2, 4, 0.5][scale]
                return np.clip(r, 0, 1) if clamp else r
            col = comb(carg(ab & 15), carg(ab >> 4), carg(cd & 15), carg(cd >> 4), op, cl & 1)
            alp = comb(aarg(aab & 7), aarg((aab >> 4) & 7), aarg(acd & 7), aarg((acd >> 4) & 7), aop, acl & 1)
            res = np.concatenate([col, alp[..., None]], -1)
            outc, outa = (cl >> 1) & 3, (acl >> 1) & 3
            tgt = [prev] + regs
            new = [t.copy() for t in tgt]
            new[outc][..., :3] = res[..., :3]
            new[outa][..., 3] = res[..., 3]
            prev, regs = new[0], new[1:]
        return np.clip(prev, 0, 1)


# ------------------------------------------------------------------ BRLYT
class Pane:
    def __init__(self, kind, b, o):
        self.kind = kind
        self.flag, self.base, self.alpha = b[o + 8], b[o + 9], b[o + 10]
        self.name = b[o + 0xC:o + 0x1C].split(b"\0")[0].decode("latin1")
        self.user = b[o + 0x1C:o + 0x24].split(b"\0")[0].decode("latin1")
        self.tx, self.ty, self.tz = struct.unpack_from("<3f", b, o + 0x24)
        self.rx, self.ry, self.rz = struct.unpack_from("<3f", b, o + 0x30)
        self.scx, self.scy = struct.unpack_from("<2f", b, o + 0x3C)
        self.w, self.h = struct.unpack_from("<2f", b, o + 0x44)
        self.children = []
        self.visible = bool(self.flag & 1)
        self.text_override = None


class Layout:
    def __init__(self, arc, name, fonts):
        self.arc = arc
        lyt = arc[[k for k in arc if k.endswith(name.lower() + ".brlyt")][0]]
        b = lyt
        o = u16(b, 12)
        n = u16(b, 14)
        textures, fontnames, self.mats = [], [], []
        stack, last = [], None
        self.root = None
        self.panes = {}
        for _ in range(n):
            tag = b[o:o + 4][::-1].decode("latin1"); size = u32(b, o + 4)
            if tag == "lyt1":
                self.width, self.height = struct.unpack_from("<2f", b, o + 12)
            elif tag in ("txl1", "fnl1"):
                cnt = u16(b, o + 8)
                base = o + 12
                names = []
                for i in range(cnt):
                    so = u32(b, base + 8 * i)
                    names.append(b[base + so:b.index(b"\0", base + so)].decode("latin1"))
                if tag == "txl1":
                    for nm in names:
                        key = [k for k in arc if k.endswith("/" + nm.lower()) or k.endswith(nm.lower())]
                        textures.append(decode_tpl(arc[key[0]]) if key else None)
                else:
                    fontnames = names
            elif tag == "mat1":
                cnt = u16(b, o + 8)
                for i in range(cnt):
                    self.mats.append(Material(b, o + u32(b, o + 12 + 4 * i), textures))
            elif tag in ("pan1", "pic1", "txt1", "wnd1", "bnd1"):
                p = Pane(tag, b, o)
                if tag == "pic1":
                    p.vtx = [np.array(list(b[o + 0x4C + 4 * i:o + 0x50 + 4 * i]), np.float32) / 255 for i in range(4)]
                    p.mat = u16(b, o + 0x5C); ntc = b[o + 0x5E]
                    p.tcs = [struct.unpack_from("<8f", b, o + 0x60 + 32 * i) for i in range(ntc)]
                elif tag == "txt1":
                    q = o + 0x4C
                    p.bufb, p.strb, p.mat, p.font = u16(b, q), u16(b, q + 2), u16(b, q + 4), u16(b, q + 6)
                    p.tpos, p.talign = b[q + 8], b[q + 9]
                    so = u32(b, q + 12)
                    p.cols = [np.array(list(b[q + 16:q + 20]), np.float32) / 255, np.array(list(b[q + 20:q + 24]), np.float32) / 255]
                    p.fsx, p.fsy, p.cspace, p.lspace = struct.unpack_from("<4f", b, q + 24)
                    p.text = b[o + so:o + so + p.strb].decode("utf-16-le", "replace").rstrip("\0")
                    p.fontname = fontnames[p.font] if p.font < len(fontnames) else ""
                elif tag == "wnd1":
                    q = o + 0x4C
                    p.infl = struct.unpack_from("<4f", b, q)
                    nfr = b[q + 16]
                    co = o + u32(b, q + 20); fo = o + u32(b, q + 24)
                    p.cvtx = [np.array(list(b[co + 4 * i:co + 4 * i + 4]), np.float32) / 255 for i in range(4)]
                    p.cmat = u16(b, co + 16)
                    ntc = b[co + 18]
                    p.ctcs = [struct.unpack_from("<8f", b, co + 20 + 32 * i) for i in range(ntc)]
                    p.frames = []
                    for i in range(nfr):
                        fr = o + u32(b, fo + 4 * i)
                        p.frames.append((u16(b, fr), b[fr + 2]))
                self.panes[p.name] = p
                if stack: stack[-1].children.append(p)
                else: self.root = p
                last = p
            elif tag == "pas1":
                stack.append(last)
            elif tag == "pae1":
                stack.pop()
            o += size
        self.fonts = fonts

    # ---- control
    def hide(self, *names):
        for n in names:
            if n in self.panes: self.panes[n].visible = False

    def show(self, *names):
        for n in names:
            if n in self.panes: self.panes[n].visible = True

    def only(self, keep):
        """hide the root's children except the named ones"""
        for c in self.root.children:
            c.visible = c.name in keep

    def set_text(self, pane, text):
        """a text box, or every text box under a pane (as MR::setTextBoxMessageRecursive)"""
        def rec(p):
            if p.kind == "txt1": p.text_override = text
            for c in p.children: rec(c)
        if pane in self.panes: rec(self.panes[pane])

    # ---- drawing
    def draw(self, cv, tx=0.0, ty=0.0, alpha=1.0, wide=True, scale=1.0):
        """the layout with its root moved by (tx, ty) layout units (y up). 16:9: 608 units across the picture, flagged panes
        squeezed by 0.75 in x (nw4r location adjust)."""
        self.cv, self.wide = cv, wide
        self.ux = cv.W / self.width if wide else 1.0   # square units per layout unit (x)
        m = np.array([[scale, 0, tx], [0, scale, ty]], np.float64)
        self._draw(self.root, m, alpha)

    def _draw(self, p, parent, alpha):
        if not p.visible: return
        sx, sy = p.scx, p.scy
        if self.wide and (p.flag & 4): sx *= 0.75
        rz = math.radians(p.rz)
        c, s = math.cos(rz), math.sin(rz)
        local = np.array([[c * sx, -s * sy, p.tx], [s * sx, c * sy, p.ty]], np.float64)
        m = np.vstack([parent, [0, 0, 1]]) @ np.vstack([local, [0, 0, 1]])
        m = m[:2]
        a = alpha * p.alpha / 255.0 if (p.flag & 2) else p.alpha / 255.0 * (alpha if p is self.root else 1.0)
        a = alpha * p.alpha / 255.0
        # the pane's rect (base position: h = base % 3, v = base // 3; y up)
        hb, vb = p.base % 3, p.base // 3
        x0 = [0, -p.w / 2, -p.w][hb]; y0 = [0, p.h / 2, p.h][vb]
        rect = (x0, y0, x0 + p.w, y0 - p.h)  # left, top, right, bottom (layout, y up)
        if p.kind == "pic1": self._pic(p, m, rect, a)
        elif p.kind == "wnd1": self._wnd(p, m, rect, a)
        elif p.kind == "txt1": self._txt(p, m, rect, a)
        for ch in p.children: self._draw(ch, m, a)

    def _to_px(self, m, x, y):
        X = m[0, 0] * x + m[0, 1] * y + m[0, 2]
        Y = m[1, 0] * x + m[1, 1] * y + m[1, 2]
        sqx = self.cv.W / 2 + X * self.ux
        sqy = self.cv.H / 2 - Y
        return sqx * self.cv.sx, sqy * self.cv.sy

    def _quad(self, m, l, t, r, bt, mat, vtx, tcs, a):
        """vtx: 4 colours TL TR BL BR; tcs: list of (8 floats: TL TR BL BR s,t)"""
        X0, Y0 = self._to_px(m, l, t); X1, Y1 = self._to_px(m, r, bt)
        flipx, flipy = X1 < X0, Y1 < Y0

        def shader(u, v):
            if flipx: u = 1 - u
            if flipy: v = 1 - v
            c = [np.asarray(q, np.float32) for q in vtx]
            ras = ((c[0] * (1 - u[..., None]) + c[1] * u[..., None]) * (1 - v[..., None]) +
                   (c[2] * (1 - u[..., None]) + c[3] * u[..., None]) * v[..., None])
            if mat.chan_src_reg and mat.matcol is not None:
                ras = np.broadcast_to(mat.matcol, ras.shape).copy()
            ras = ras.copy(); ras[..., 3] *= a
            coords = []
            for tc in tcs or [(0, 0, 1, 0, 0, 1, 1, 1)]:
                s0, t0, s1, t1, s2, t2, s3, t3 = tc
                uu = (s0 * (1 - u) + s1 * u) * (1 - v) + (s2 * (1 - u) + s3 * u) * v
                vv = (t0 * (1 - u) + t1 * u) * (1 - v) + (t2 * (1 - u) + t3 * u) * v
                coords.append((uu, vv))
            return mat.shade(ras, coords)
        self.cv.shade_quad((X0, Y0, X1, Y1), shader)

    def _pic(self, p, m, rect, a):
        mat = self.mats[p.mat]
        l, t, r, b = rect
        self._quad(m, l, t, r, b, mat, p.vtx, p.tcs, a)

    def _wnd(self, p, m, rect, a):
        l, t, r, b = rect
        il, ir, it, ib = p.infl
        cmat = self.mats[p.cmat]
        mats0 = [self.mats[f[0]] for f in p.frames]
        tex00 = mats0[0].texmaps[0][0] if mats0 and mats0[0].texmaps else None
        fw0 = tex00.shape[1] if tex00 is not None else 0
        fh0 = tex00.shape[0] if tex00 is not None else 0
        # (nw4r Window: the content fills the inside of the frame, grown by the inflation)
        self._quad(m, l + fw0 - il, t - fh0 + it, r - fw0 + ir, b + fh0 - ib, cmat, p.cvtx, p.ctcs, a)
        if not p.frames: return
        white = [np.ones(4, np.float32)] * 4
        mats = [self.mats[f[0]] for f in p.frames]
        tex0 = mats[0].texmaps[0][0] if mats[0].texmaps else None
        fw = tex0.shape[1] if tex0 is not None else 16
        fh = tex0.shape[0] if tex0 is not None else 16
        # corners: TL, TR (flip h), BL (flip v), BR (flip both) when one material; 4 materials: one each (nw4r Window)
        corners = [(l, t, l + fw, t - fh, (0, 0, 1, 0, 0, 1, 1, 1)), (r - fw, t, r, t - fh, (1, 0, 0, 0, 1, 1, 0, 1)),
                   (l, b + fh, l + fw, b, (0, 1, 1, 1, 0, 0, 1, 0)), (r - fw, b + fh, r, b, (1, 1, 0, 1, 1, 0, 0, 0))]
        for i, (x0, y0, x1, y1, tc) in enumerate(corners):
            mat = mats[i] if len(mats) == 4 else mats[0]
            if len(mats) == 4: tc = (0, 0, 1, 0, 0, 1, 1, 1)
            self._quad(m, x0, y0, x1, y1, mat, white, [tc], a)
        # edges: the corner texture's inner column / row stretched
        mat = mats[0]
        e = 0.999
        self._quad(m, l + fw, t, r - fw, t - fh, mat, white, [(e, 0, e, 0, e, 1, e, 1)], a)            # top
        self._quad(m, l + fw, b + fh, r - fw, b, mat, white, [(e, 1, e, 1, e, 0, e, 0)], a)            # bottom
        self._quad(m, l, t - fh, l + fw, b + fh, mat, white, [(0, e, 1, e, 0, e, 1, e)], a)            # left
        self._quad(m, r - fw, t - fh, r, b + fh, mat, white, [(1, e, 0, e, 1, e, 0, e)], a)            # right

    def _txt(self, p, m, rect, a):
        text = p.text_override if p.text_override is not None else p.text
        if not text: return
        font = self.fonts.get(p.fontname.lower()) or self.fonts.get("messagefont26.brfnt")
        if font is None: return
        mat = self.mats[p.mat]
        l, t, r, b = rect
        sc = p.fsy / font.height
        scx = p.fsx / font.width if font.width else sc
        lines = text.split("\n")
        lh = p.fsy + p.lspace
        total = lh * len(lines) - p.lspace
        hpos, vpos = p.tpos % 3, p.tpos // 3
        y = [t, (t + b) / 2 + total / 2, b + total][vpos]
        col_top, col_bot = p.cols
        white = mat.tevc[1]
        for line in lines:
            # (picture tags have been turned into private-use characters by the caller: U+E000 + n)
            w = sum((font.advance(ord(ch)) if not (0xE000 <= ord(ch) < 0xE100) else 34) * scx + p.cspace for ch in line) - p.cspace
            x = [l, (l + r) / 2 - w / 2, r - w][hpos if p.talign == 0 else (p.talign - 1)]
            for ch in line:
                code = ord(ch)
                f = font
                if 0xE000 <= code < 0xE100 and "picturefont.brfnt" in self.fonts:
                    f = self.fonts["picturefont.brfnt"]; code = 0x30 + code - 0xE000
                g, tex, left, gw, cw = f.glyph(code)
                gsc = p.fsy / f.height
                if gw > 0:
                    gx0 = x + left * gsc; gx1 = gx0 + gw * gsc
                    X0, Y0 = self._to_px(m, gx0, y); X1, Y1 = self._to_px(m, gx1, y - f.ch * gsc)
                    is_pic = f is not font or f.fmt != 0

                    def shader(u, v, tex=tex, is_pic=is_pic):
                        s = sample(tex, u, v)
                        c = (col_top * (1 - v[..., None]) + col_bot * v[..., None])
                        if is_pic:
                            out = s * c
                        else:
                            out = np.concatenate([np.broadcast_to(white[:3] * c[..., :3], s[..., :3].shape), (s[..., 3] * c[..., 3])[..., None]], -1)
                        out = out.copy(); out[..., 3] *= a
                        return out
                    self.cv.shade_quad((X0, Y0, X1, Y1), shader)
                x += cw * gsc + p.cspace
            y -= lh


def load_fonts(font_arc, patched=None):
    fonts = {}
    for k, v in font_arc.items():
        if k.endswith(".brfnt"):
            name = k.split("/")[-1]
            fonts[name] = RFont(v)
    if patched and "picturefont.brfnt" in fonts:
        fonts["picturefont.brfnt"].patched = patched
    return fonts


# ------------------------------------------------------------------ BRLAN (animations: the value at one frame)
def _hermite(keys, f):
    if not keys: return None
    if f <= keys[0][0]: return keys[0][1]
    if f >= keys[-1][0]: return keys[-1][1]
    for (f0, v0, s0), (f1, v1, s1) in zip(keys, keys[1:]):
        if f0 <= f <= f1:
            if f1 == f0: return v1
            t = (f - f0) / (f1 - f0); d = f1 - f0
            h00 = 2 * t ** 3 - 3 * t ** 2 + 1; h10 = t ** 3 - 2 * t ** 2 + t; h01 = -2 * t ** 3 + 3 * t ** 2; h11 = t ** 3 - t ** 2
            return h00 * v0 + h10 * d * s0 + h01 * v1 + h11 * d * s1
    return keys[-1][1]


def _step(keys, f):
    v = keys[0][1]
    for k in keys:
        if k[0] <= f: v = k[1]
    return v


class Anim:
    def __init__(self, b):
        o = u16(b, 12)
        self.frames, self.entries, self.files = 0, [], []
        for _ in range(u16(b, 14)):
            tag = b[o:o + 4][::-1]; size = u32(b, o + 4)
            if tag == b"pai1":
                self.frames = u16(b, o + 8)
                nfiles, ncont, contoff = u16(b, o + 12), u16(b, o + 14), u32(b, o + 16)
                for i in range(nfiles):
                    so = u32(b, o + 20 + 4 * i)
                    self.files.append(b[o + 20 + so:b.index(b"\0", o + 20 + so)].decode("latin1"))
                for i in range(ncont):
                    co = o + u32(b, o + contoff + 4 * i)
                    name = b[co:co + 20].split(b"\0")[0].decode("latin1")
                    num, typ = b[co + 20], b[co + 21]
                    for k in range(num):
                        io = co + u32(b, co + 24 + 4 * k)
                        kind = b[io:io + 4][::-1].decode("latin1")
                        for t in range(b[io + 4]):
                            to = io + u32(b, io + 8 + 4 * t)
                            idx, target, curve = b[to], b[to + 1], b[to + 2]
                            nk = u16(b, to + 4); ko = to + u32(b, to + 8)
                            if curve == 2:
                                keys = [struct.unpack_from("<3f", b, ko + 12 * j) for j in range(nk)]
                            else:
                                keys = [(f32(b, ko + 8 * j), u16(b, ko + 8 * j + 4), 0) for j in range(nk)]
                            self.entries.append((name, typ, kind, idx, target, curve, keys))
            o += size

    def apply(self, lay, frame, under=None):
        """set the layout's panes / materials to this frame (only panes under `under`, as startPaneAnim binds)"""
        allowed = None
        if under:
            allowed = set()
            def rec(p):
                allowed.add(p.name)
                for c in p.children: rec(c)
            if under in lay.panes: rec(lay.panes[under])
        mats = {m.name: m for m in lay.mats}
        for name, typ, kind, idx, target, curve, keys in self.entries:
            v = _hermite(keys, frame) if curve == 2 else _step(keys, frame)
            if v is None: continue
            if typ == 0:
                p = lay.panes.get(name)
                if p is None or (allowed is not None and name not in allowed): continue
                if kind == "RLPA":
                    attr = ["tx", "ty", "tz", "rx", "ry", "rz", "scx", "scy", "w", "h"][target]
                    setattr(p, attr, v)
                elif kind == "RLVC":
                    if target == 16: p.alpha = int(max(0, min(255, round(v))))
                    elif hasattr(p, "vtx") and target < 16: p.vtx[target // 4][target % 4] = max(0, min(255, v)) / 255
                    elif hasattr(p, "cvtx") and target < 16: p.cvtx[target // 4][target % 4] = max(0, min(255, v)) / 255
                elif kind == "RLVI":
                    p.visible = bool(v)
            else:
                m = mats.get(name)
                if m is None: continue
                if allowed is not None and not any(getattr(q, "mat", None) is not None and lay.mats[q.mat] is m for q in lay.panes.values() if q.name in allowed):
                    continue
                if kind == "RLMC":
                    val = max(0, min(255, v)) / 255
                    if target < 4:
                        if m.matcol is None: m.matcol = np.ones(4, np.float32)
                        m.matcol[target] = val
                    elif target < 16:
                        m.tevc[(target - 4) // 4][(target - 4) % 4] = v / 255
                    elif target < 32:
                        m.kc[(target - 16) // 4][(target - 16) % 4] = val
                elif kind == "RLTP":
                    fname = self.files[int(v)] if int(v) < len(self.files) else None
                    key = [k for k in lay.arc if fname and k.endswith(fname.lower())]
                    if key and idx < len(m.texmaps):
                        t = m.texmaps[idx]
                        m.texmaps[idx] = (decode_tpl(lay.arc[key[0]]), t[1], t[2])


def anim(lay, name):
    key = [k for k in lay.arc if k.endswith("/" + name.lower() + ".brlan")]
    return Anim(lay.arc[key[0]]) if key else None


def pose(lay, name, frame=None, under=None):
    a = anim(lay, name)
    if a is None: return False
    a.apply(lay, a.frames if frame is None else frame, under)
    return True


LANG4 = ["JpJa", "UsEn", "UsSp", "UsFr", "EuEn", "EuSp", "EuFr", "EuGe", "EuIt", "EuDu", "CnSi", "KrKo"]


def localize(lay, prefix="UsEnglish"):
    """LayoutManager's language panes: if a parent has children for the current language, those stay (renamed without the
    suffix) and every other child goes; else the other languages' children go."""
    cur = prefix[:4]

    def rec(p):
        langs = [c for c in p.children if len(c.name) >= 4 and c.name[-4:] in LANG4]
        if any(c.name[-4:] == cur for c in langs):
            keep = []
            for c in p.children:
                if c.name[-4:] == cur and c in langs:
                    lay.panes.pop(c.name, None)
                    c.name = c.name[:-4]
                    lay.panes[c.name] = c
                    keep.append(c)
            p.children = keep
        else:
            p.children = [c for c in p.children if c not in langs]
        for c in p.children: rec(c)
    rec(lay.root)

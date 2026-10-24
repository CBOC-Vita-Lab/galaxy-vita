"""GX texture decoders (numpy): level-0 texel bytes (big-endian, tiled) -> RGBA8 array (h, w, 4).
Mirrors platform/vgx/src/vgx_texdecode.cpp (I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR)."""
import numpy as np

from gxarc import FMT


def _untile(blocks, w, h, bw, bh):
    """blocks: (by*bx, bh, bw, C) in tile order -> (h, w, C) cropped."""
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = blocks.reshape(by, bx, bh, bw, -1).transpose(0, 2, 1, 3, 4).reshape(by * bh, bx * bw, -1)
    return a[:h, :w]


def _expand(v, bits):
    v = v.astype(np.uint32)
    if bits == 3:
        return ((v << 5) | (v << 2) | (v >> 1)).astype(np.uint8)
    if bits == 4:
        return ((v << 4) | v).astype(np.uint8)
    if bits == 5:
        return ((v << 3) | (v >> 2)).astype(np.uint8)
    if bits == 6:
        return ((v << 2) | (v >> 4)).astype(np.uint8)
    return v.astype(np.uint8)


def _rgb565(v):
    return np.stack([_expand((v >> 11) & 31, 5), _expand((v >> 5) & 63, 6), _expand(v & 31, 5), np.full(v.shape, 255, np.uint8)], -1)


def _rgb5a3(v):
    hi = (v & 0x8000) != 0
    r = np.where(hi, _expand((v >> 10) & 31, 5), _expand((v >> 8) & 15, 4))
    g = np.where(hi, _expand((v >> 5) & 31, 5), _expand((v >> 4) & 15, 4))
    b = np.where(hi, _expand(v & 31, 5), _expand(v & 15, 4))
    a = np.where(hi, 255, _expand((v >> 12) & 7, 3))
    return np.stack([r, g, b, a], -1).astype(np.uint8)


def _ia8(v):
    i = (v & 0xFF).astype(np.uint8)
    a = (v >> 8).astype(np.uint8)
    return np.stack([i, i, i, a], -1)


def _palette(pal, palFmt):
    v = np.frombuffer(pal, '>u2').astype(np.uint32)
    if palFmt == 0:
        return _ia8(v)
    if palFmt == 1:
        return _rgb565(v)
    return _rgb5a3(v)


def decode(fmt, w, h, data, pal=b'', palFmt=0):
    bw, bh, bpp = FMT[fmt]
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    nb = bx * by
    d = np.frombuffer(data, np.uint8)
    if fmt == 0xE:
        return _untile(_cmpr_blocks(d, nb), w, h, 8, 8)
    if fmt in (0, 8):  # 4-bit
        t = d.reshape(nb, bh, bw // 2)
        v = np.stack([t >> 4, t & 15], -1).reshape(nb, bh, bw)
        if fmt == 0:
            i = _expand(v, 4)
            px = np.stack([i, i, i, i], -1)
        else:
            px = _palette(pal, palFmt)[np.minimum(v, max(len(pal) // 2 - 1, 0))]
        return _untile(px, w, h, bw, bh)
    if fmt in (1, 9):
        v = d.reshape(nb, bh, bw)
        if fmt == 1:
            px = np.stack([v, v, v, v], -1)
        else:
            px = _palette(pal, palFmt)[np.minimum(v, max(len(pal) // 2 - 1, 0))]
        return _untile(px, w, h, bw, bh)
    if fmt == 2:
        v = d.reshape(nb, bh, bw)
        i = _expand(v & 15, 4)
        a = _expand(v >> 4, 4)
        return _untile(np.stack([i, i, i, a], -1), w, h, bw, bh)
    if fmt in (3, 4, 5, 0xA):
        v = np.frombuffer(data, '>u2').astype(np.uint32).reshape(nb, bh, bw)
        if fmt == 3:
            # IA8 texel: alpha byte first, then intensity
            px = np.stack([v & 0xFF, v & 0xFF, v & 0xFF, v >> 8], -1).astype(np.uint8)
        elif fmt == 4:
            px = _rgb565(v)
        elif fmt == 5:
            px = _rgb5a3(v)
        else:
            px = _palette(pal, palFmt)[np.minimum(v & 0x3FFF, max(len(pal) // 2 - 1, 0))]
        return _untile(px, w, h, bw, bh)
    if fmt == 6:
        t = d.reshape(nb, 2, 16, 2)  # AR plane then GB plane, 16 texels each
        a, r = t[:, 0, :, 0], t[:, 0, :, 1]
        g, b = t[:, 1, :, 0], t[:, 1, :, 1]
        px = np.stack([r, g, b, a], -1).reshape(nb, 4, 4, 4)
        return _untile(px, w, h, 4, 4)
    raise ValueError('format %d' % fmt)


def _cmpr_blocks(d, nb):
    """CMPR: 8x8 tiles of four 4x4 DXT1-like sub-blocks (big-endian colours, 2-bit indices MSB first)."""
    s = d.reshape(nb, 4, 8)
    c0 = (s[:, :, 0].astype(np.uint32) << 8) | s[:, :, 1]
    c1 = (s[:, :, 2].astype(np.uint32) << 8) | s[:, :, 3]
    p0 = _rgb565(c0).astype(np.int32)
    p1 = _rgb565(c1).astype(np.int32)
    gt = (c0 > c1)[..., None]
    p2 = np.where(gt, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(gt, (p0 + 2 * p1) // 3, 0)
    p2[..., 3] = 255
    p3[..., 3] = np.where(gt[..., 0], 255, 0)
    pal = np.stack([p0, p1, p2, p3], 2).astype(np.uint8)  # (nb, 4 sub, 4 colours, 4)
    idx = s[:, :, 4:8]  # (nb, 4, 4 rows)
    sel = np.stack([(idx >> (6 - 2 * x)) & 3 for x in range(4)], -1)  # (nb, 4sub, 4 rows, 4 cols)
    n = np.arange(nb)[:, None, None, None]
    sb = np.arange(4)[None, :, None, None]
    px = pal[n, sb, sel]  # (nb, sub, 4 rows, 4 cols, 4); sub-blocks are laid out 2x2 inside the 8x8 tile
    px = px.reshape(nb, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(nb, 8, 8, 4)
    return px

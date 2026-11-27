#!/usr/bin/env python3
"""PETARI_CLIPCHAIN_ONEPASS: numpy model of the clip field's soft edge (FallOutFieldDraw::draw) and of the one-pass replacements.

    clipblur_model.py [--png OUT.png] [--K 6]

The chain (640x456 virtual EFB): tex2 = A8 capture of the EFB's alpha, 2x box (320x228); the mask M = 255 where tex2 says outside (the compare of the quarter
draw); tex3 = box2(M) (160x114); tex4 = saturating sum of 5 bilinear taps of tex3 on a ring of radius 0.004 UV (a 160x114 tile); the edge = tex4 magnified
bilinearly 4x, alpha = tex4 * 128/255.   Mode 1: K thresholded taps of tex2 straight at the EFB pixel (the edge draw does it).   Mode 2: the same K taps, drawn
into the tile (160x114) and magnified as the chain's tex4 is.  Tap rings = the tables of ImageEffectLocalUtil.cpp (fitted on hard-edged masks at 4 grid phases).
Prints, per synthetic mask, max / mean |new edge - chain edge| in 8-bit levels of the edge alpha (0..128) over the whole picture and over the band
(either edge > 0 and < 128), at 4 grid phases."""
import argparse
import sys
import numpy as np

W, H = 640, 456
RING1 = {4: (6.00, 0.785, 2.00), 5: (5.54, 0.028, 2.37), 6: (5.88, 0.729, 2.09), 7: (5.70, 0.800, 2.20), 8: (5.55, 0.895, 2.27)}
RING2 = {4: (5.92, 0.884, 2.05), 5: (5.07, 0.836, 2.50), 6: (4.97, 0.815, 2.83), 7: (5.00, 0.900, 2.70), 8: (5.04, 0.965, 2.53)}


def box2(a):
    return (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) / 4.0


def q8(a):
    return np.clip(np.floor(a + 0.5), 0, 255)


def bil(tex, x, y):  # bilinear, clamp to edge; x, y in texels, centre at +0.5
    h, w = tex.shape
    fx, fy = x - 0.5, y - 0.5
    x0, y0 = np.floor(fx).astype(int), np.floor(fy).astype(int)
    ax, ay = fx - x0, fy - y0
    g = lambda xx, yy: tex[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)]
    return g(x0, y0) * (1 - ax) * (1 - ay) + g(x0 + 1, y0) * ax * (1 - ay) + g(x0, y0 + 1) * (1 - ax) * ay + g(x0 + 1, y0 + 1) * ax * ay


def chain(field, inside=False, aspect=1.333, radius=0.004, n=5):
    tex2 = q8(box2(field))
    m = 255.0 * ((tex2 >= 1) if inside else (tex2 < 1))
    t3 = q8(box2(m))
    h, w = t3.shape
    yy, xx = np.mgrid[0:h, 0:w]
    acc = None
    for i in range(n):
        a = 2 * np.pi * i / n
        v = q8(bil(t3, xx + 0.5 + radius * np.cos(a) * w, yy + 0.5 + radius * aspect * np.sin(a) * h))
        acc = v if acc is None else np.clip(acc + v, 0, 255)
    return tex2, acc


def edge_chain(t4):
    h, w = t4.shape
    Y, X = np.mgrid[0:H, 0:W]
    return bil(t4, (X + 0.5) * w / W, (Y + 0.5) * h / H) * 128.0 / 255.0


def taps_of(K, ring):
    rho, rot, gain = ring[K]
    return [(rho * np.cos(rot + 2 * np.pi * k / K), rho * np.sin(rot + 2 * np.pi * k / K)) for k in range(K)], gain


def thresholded(tex2, px, py, taps, gain, inside):  # sum of K taps (EFB px offsets), each the compare of tex2 with 1/255
    h2, w2 = tex2.shape
    acc = np.zeros(px.shape)
    for dx, dy in taps:
        v = bil(tex2, (px + dx) / 2.0, (py + dy) / 2.0)
        acc += (v > 1.0) if inside else (v < 1.0)
    return np.clip(acc / len(taps) * gain, 0, 1)


def mode1(tex2, K, inside):
    taps, gain = taps_of(K, RING1)
    Y, X = np.mgrid[0:H, 0:W]
    return 128.0 * thresholded(tex2, X + 0.5, Y + 0.5, taps, gain, inside)


def mode2(tex2, K, inside):
    taps, gain = taps_of(K, RING2)
    yy, xx = np.mgrid[0:114, 0:160]
    t4 = q8(255.0 * thresholded(tex2, (xx + 0.5) * 4, (yy + 0.5) * 4, taps, gain, inside))
    return edge_chain(t4)


def shapes():
    Y, X = np.mgrid[0:H, 0:W]
    out = {}
    f = np.zeros((H, W)); f[((X - 320) ** 2 + (Y - 228) ** 2) < 120 ** 2] = 4; out['circle'] = f
    f = np.zeros((H, W)); f[100:360, 150:470] = 4; out['rect'] = f
    f = np.zeros((H, W)); f[(Y - 0.35 * X) > 20] = 4; out['diagonal'] = f
    f = np.zeros((H, W)); f[220:226, 40:600] = 4; f[40:420, 300:305] = 4; out['slits'] = f
    f = np.zeros((H, W))
    for cx, cy, r in [(100, 100, 9), (200, 300, 5), (500, 120, 14), (560, 380, 3)]:
        f[((X - cx) ** 2 + (Y - cy) ** 2) < r * r] = 4
    f[((X - 320) ** 2 + (Y - 228) ** 2) < 60 ** 2] = 8
    out['blobs'] = f
    return out


def png(a, path):
    import struct, zlib
    a = np.clip(a, 0, 255).astype(np.uint8)
    if a.ndim == 2:
        a = np.repeat(a[:, :, None], 3, 2)
    h, w, _ = a.shape
    raw = b''.join(b'\0' + a[y].tobytes() for y in range(h))
    def ch(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--png')
    ap.add_argument('--K', type=int, default=6)
    o = ap.parse_args()
    print('K=%d; levels of the edge alpha (0..128); band = pixels where either edge is in (0,128)' % o.K)
    print('%-9s %-4s %-6s %8s %8s %9s %9s' % ('mask', 'mode', 'side', 'max', 'mean', 'band max', 'band mean'))
    rows = []
    for name, f in shapes().items():
        for inside in (False, True):
            res = {1: [], 2: []}
            for ox, oy in ((0, 0), (1, 0), (0, 1), (3, 2)):  # grid phases
                g = np.roll(np.roll(f, ox, 1), oy, 0)
                tex2, t4 = chain(g, inside)
                e0 = edge_chain(t4)
                for mode, fn in ((1, mode1), (2, mode2)):
                    e1 = fn(tex2, o.K, inside)
                    d = np.abs(e1 - e0)
                    band = ((e0 > 0) & (e0 < 128)) | ((e1 > 0) & (e1 < 128))
                    res[mode].append((d.max(), d.mean(), d[band].max() if band.any() else 0, d[band].mean() if band.any() else 0))
                    if (ox, oy) == (0, 0) and not inside:
                        rows.append((name, mode, e0, e1))
            for mode in (1, 2):
                r = np.array(res[mode])
                print('%-9s %-4d %-6s %8.1f %8.3f %9.1f %9.2f' % (name, mode, 'inside' if inside else 'outside', r[:, 0].max(), r[:, 1].mean(), r[:, 2].max(), r[:, 3].mean()))
    if o.png:  # chain | mode 1 | mode 2 | |diff mode 2| x4, per mask (edge alpha as grey, x2)
        tiles = []
        for name in shapes():
            e0 = [r for r in rows if r[0] == name and r[1] == 1][0][2]
            e1 = [r for r in rows if r[0] == name and r[1] == 1][0][3]
            e2 = [r for r in rows if r[0] == name and r[1] == 2][0][3]
            tiles.append(np.concatenate([x[::2, ::2] * 2 for x in (e0, e1, e2, np.abs(e2 - e0) * 4)], axis=1))
        png(np.concatenate(tiles, axis=0), o.png)


if __name__ == '__main__':
    main()

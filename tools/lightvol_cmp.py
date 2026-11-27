#!/usr/bin/env python3
"""Compare the RGBA8 pictures of two tests/lightvol_gl runs (LV_DUMPDIR).  lightvol_cmp.py A B [--png OUT]
Per frame: identical or not, differing pixel count, PSNR of RGB and of ALPHA (inf when equal).  Exit 0 always; the script prints one line per frame:
  frame f01: IDENTICAL                      or
  frame f01: DIFF px=1234 rgb_psnr=41.2 a_psnr=inf maxdiff=17
"""
import sys, os, zlib, struct
import numpy as np

def load(path):
    w, h = [int(v) for v in os.path.basename(path).split('-')[1].split('.')[0].split('x')]
    return np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)

def psnr(a, b):
    d = a.astype(np.float64) - b.astype(np.float64)
    m = (d * d).mean()
    return float('inf') if m == 0 else 10 * np.log10(255.0 * 255.0 / m)

def png(path, img):
    h, w, _ = img.shape
    raw = b''.join(b'\x00' + img[y, :, :3].tobytes() for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def steps(d):
    """--steps DIR: consecutive pictures of one run: differing pixels, the largest per-channel step and RGB PSNR (a fade is small steps, a pop one big one)."""
    prev = None
    for f in sorted(os.listdir(d)):
        if not f.endswith('.rgba'):
            continue
        cur = load(os.path.join(d, f))
        if prev is not None:
            print('step %s: px=%d maxstep=%d rgb_psnr=%.1f' % (f[:3], int((cur != prev).any(axis=2).sum()), int(np.abs(cur.astype(int) - prev.astype(int)).max()), psnr(cur[:, :, :3], prev[:, :, :3])))
        prev = cur

def main():
    if sys.argv[1] == '--steps':
        steps(sys.argv[2]); return
    a, b = sys.argv[1], sys.argv[2]
    out = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--png' else None
    for fa in sorted(os.listdir(a)):
        if not fa.endswith('.rgba'):
            continue
        pa = load(os.path.join(a, fa))
        fb = os.path.join(b, fa)
        if not os.path.exists(fb):
            print('frame %s: MISSING' % fa[:3]); continue
        pb = load(fb)
        if pa.shape != pb.shape:
            print('frame %s: SHAPE' % fa[:3]); continue
        if np.array_equal(pa, pb):
            print('frame %s: IDENTICAL' % fa[:3])
        else:
            nd = int((pa != pb).any(axis=2).sum())
            print('frame %s: DIFF px=%d rgb_psnr=%.1f a_psnr=%s maxdiff=%d' % (fa[:3], nd, psnr(pa[:, :, :3], pb[:, :, :3]),
                  ('inf' if np.array_equal(pa[:, :, 3], pb[:, :, 3]) else '%.1f' % psnr(pa[:, :, 3], pb[:, :, 3])), int(np.abs(pa.astype(int) - pb.astype(int)).max())))
        if out:
            png(os.path.join(out, 'a-' + fa[:3] + '.png'), pa); png(os.path.join(out, 'b-' + fa[:3] + '.png'), pb)

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""GXV_BLOOM_FAST: compare the pictures of two runs of tests/bloom_fast_gl (BF_DUMPDIR dumps, RGBA8 at the render size).

    bloom_fast_cmp.py REF_DIR TEST_DIR [--scene SCENE_DIR] [--png OUT_PREFIX] [--min-psnr DB]

Per picture: PSNR of the whole picture (RGB), the largest channel difference, the share of pixels off by more than 2 / 8 / 16 levels.
With --scene (a run without glow sources: the scene alone) also the glow's own figures: glow = picture - scene, PSNR of the test
glow against the reference glow, and the glow's total light (sum of the glow) of the test as a share of the reference's.
--png writes REF | TEST | |diff| x 8 side by side (and the glow alone when --scene is given) for a look.
Exit 1 when a picture's PSNR is below --min-psnr (default 0: report only)."""
import argparse
import glob
import math
import os
import sys

import numpy as np


def load(path):
    base = os.path.basename(path)
    size = base.split('-')[1].split('.')[0]
    w, h = (int(v) for v in size.split('x'))
    a = np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)
    return a[:, :, :3].astype(np.int32)  # (DebugReadTexture: rows top first)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return float('inf') if mse == 0 else 10 * math.log10(255.0 * 255.0 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref')
    ap.add_argument('test')
    ap.add_argument('--scene')
    ap.add_argument('--png')
    ap.add_argument('--min-psnr', type=float, default=0.0)
    o = ap.parse_args()
    bad = 0
    rows = []
    for rp in sorted(glob.glob(os.path.join(o.ref, 'f*-*.rgba'))):
        name = os.path.basename(rp)
        tp = os.path.join(o.test, name)
        if not os.path.exists(tp):
            print('missing', tp)
            bad += 1
            continue
        r, t = load(rp), load(tp)
        d = np.abs(r - t)
        dm = d.max(axis=2)
        p = psnr(r, t)
        line = '%s: PSNR %s dB, max diff %d, pixels off >2: %.3f%%, >8: %.3f%%, >16: %.3f%%' % (
            name, 'inf' if p == float('inf') else '%.2f' % p, int(d.max()), 100.0 * np.mean(dm > 2), 100.0 * np.mean(dm > 8), 100.0 * np.mean(dm > 16))
        gr = gt = None
        if o.scene:
            s = load(os.path.join(o.scene, name))
            gr, gt = np.clip(r - s, 0, 255), np.clip(t - s, 0, 255)
            gp = psnr(gr, gt)
            light = float(gt.sum()) / max(1.0, float(gr.sum()))
            line += '; glow: PSNR %s dB, light %.1f%% of the reference, glow pixels %.1f%%' % (
                'inf' if gp == float('inf') else '%.2f' % gp, 100.0 * light, 100.0 * np.mean(gr.max(axis=2) > 2))
        print(line)
        rows.append(p)
        if p < o.min_psnr:
            bad += 1
        if o.png:
            from PIL import Image
            parts = [r, t, np.clip(d * 8, 0, 255)]
            if gr is not None:
                parts += [np.clip(gr * 2, 0, 255), np.clip(gt * 2, 0, 255)]
            img = np.concatenate(parts, axis=1).astype(np.uint8)
            Image.fromarray(img).save('%s-%s.png' % (o.png, name.split('.')[0]))
    if rows:
        fin = [v for v in rows if v != float('inf')]
        print('pictures %d, mean PSNR %s dB, worst %s dB' % (len(rows), '%.2f' % (sum(fin) / len(fin)) if fin else 'inf',
                                                             '%.2f' % min(fin) if fin else 'inf'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

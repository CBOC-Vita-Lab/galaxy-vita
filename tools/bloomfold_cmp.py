#!/usr/bin/env python3
"""GXV_BLOOM_FOLD: compare pictures of tests/bloom_fast_gl runs (BF_DUMPDIR dumps, RGBA8).

    bloomfold_cmp.py diff REF_DIR TEST_DIR [SCENE_DIR]   max / mean per-channel difference (levels of 255), PSNR, glow light share
    bloomfold_cmp.py glow REF_DIR TEST_DIR SCENE_DIR     total glow energy and extent of a single-source picture (BF_ONE), test against reference

glow = picture - scene (clipped at 0). Energy = sum over the three channels. Extent = RMS radius about the glow's centroid (energy weighted)
and r90 (radius that holds 90% of the energy), in pixels of the dump. Prints one line per picture and a last line starting "RESULT"."""
import glob
import math
import os
import sys

import numpy as np


def load(path):
    w, h = (int(v) for v in os.path.basename(path).split('-')[1].split('.')[0].split('x'))
    return np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)[:, :, :3].astype(np.int32)


def glow_of(pic, scene):
    return np.clip(pic - scene, 0, 255).astype(np.float64).sum(axis=2)


def extent(g):
    e = g.sum()
    if e <= 0:
        return 0.0, 0.0, 0.0, (0.0, 0.0)
    ys, xs = np.mgrid[0:g.shape[0], 0:g.shape[1]]
    cx, cy = (g * xs).sum() / e, (g * ys).sum() / e
    r = np.hypot(xs - cx, ys - cy)
    rms = math.sqrt((g * r * r).sum() / e)
    order = np.argsort(r.ravel())
    cum = np.cumsum(g.ravel()[order])
    r90 = float(r.ravel()[order][np.searchsorted(cum, 0.9 * e)])
    return e, rms, r90, (cx, cy)


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ('diff', 'glow'):
        print(__doc__)
        return 2
    mode, ref, test = sys.argv[1:4]
    scene = sys.argv[4] if len(sys.argv) > 4 else None
    mx = 0
    means = []
    dE, dR, dR90 = [], [], []
    for rp in sorted(glob.glob(os.path.join(ref, 'f*-*.rgba'))):
        name = os.path.basename(rp)
        r, t = load(rp), load(os.path.join(test, name))
        if mode == 'diff':
            d = np.abs(r - t)
            mse = float(np.mean(d.astype(np.float64) ** 2))
            line = '%s: max %d, mean %.4f per channel (levels of 255), PSNR %s dB' % (name, d.max(), d.mean(), 'inf' if mse == 0 else '%.2f' % (10 * math.log10(65025.0 / mse)))
            if scene:
                s = load(os.path.join(scene, name))
                gr, gt = np.clip(r - s, 0, 255).sum(), np.clip(t - s, 0, 255).sum()
                line += ', glow light %.1f%% of the reference' % (100.0 * gt / max(1.0, gr))
            print(line)
            mx = max(mx, int(d.max()))
            means.append(float(d.mean()))
        else:
            s = load(os.path.join(scene, name))
            er, rr, r9r, cr = extent(glow_of(r, s))
            et, rt, r9t, ct = extent(glow_of(t, s))
            print('%s: energy ref %.0f test %.0f (%+.1f%%); RMS radius ref %.2f test %.2f px (%+.1f%%); r90 ref %.1f test %.1f px' % (
                name, er, et, 100.0 * (et / er - 1.0), rr, rt, 100.0 * (rt / rr - 1.0), r9r, r9t))
            dE.append(100.0 * (et / er - 1.0))
            dR.append(100.0 * (rt / rr - 1.0))
            dR90.append(r9t - r9r)
    if mode == 'diff':
        print('RESULT max %d mean %.4f' % (mx, sum(means) / max(1, len(means))))
    else:
        print('RESULT energy %+.1f%% rms-radius %+.1f%% r90 %+.1fpx' % (sum(dE) / len(dE), sum(dR) / len(dR), sum(dR90) / len(dR90)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

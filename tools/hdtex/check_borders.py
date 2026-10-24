#!/usr/bin/env python3
"""Border check of a pack: for every entry, the pack's level 0 (decoded from its DXT blocks)
is area-averaged down to the original's size and compared with the original, texel by texel,
in five regions: the top, bottom, left and right border strips (one texel wide) and the
interior (two texels in from every border). Per region: mean absolute difference and signed
bias (0-255, RGB of the texels the original shows (alpha > 0), plus alpha). A texture is
flagged when a border strip is off by much more than its interior
(MAD > 2 x interior MAD + 6, or |bias| > 6): an edge artefact of the upscaling (padding,
resampling, cropping, block compression), which the GPU would show as a band.

    python3 -E -P tools/hdtex/check_borders.py <pack> [out.tsv]
Exit status 1 when anything is flagged.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402
import compare  # noqa: E402
import gxdecode  # noqa: E402

MAD_FACTOR, MAD_SLACK, BIAS_MAX = 2.0, 6.0, 6.0


def regions(h, w):
    m = np.zeros((5, h, w), bool)
    m[0, 0, :] = True
    m[1, -1, :] = True
    m[2, :, 0] = True
    m[3, :, -1] = True
    m[4, 2:-2, 2:-2] = True
    return m


def main():
    pack = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    entries = compare.read_pack(pack)
    texs = build_pack.collect(build_pack.DEFAULT_DATA, build_pack.read_selection(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'selection.txt')))
    rows, flagged = [], []
    for k, e in sorted(entries.items(), key=lambda kv: kv[1]['name']):
        t = texs[k]['t']
        orig = gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt).astype(np.float32)
        hd = compare.decode_dxt(e['data'], e['w'], e['h'], e['fmt']).astype(np.float32)
        down = np.stack([np.asarray(Image.fromarray(hd[..., c], 'F').resize((t.w, t.h), Image.BOX)) for c in range(4)], -1)
        d = down - orig
        shown = orig[..., 3] > 0
        diff = np.concatenate([np.where(shown[..., None], d[..., :3], 0), d[..., 3:]], -1)
        weight = np.concatenate([np.repeat(shown[..., None], 3, -1), np.ones(shown.shape + (1,), bool)], -1)
        stats = []
        for m in regions(t.h, t.w):
            wm = weight & m[..., None]
            n = max(1, wm.sum())
            stats.append((np.abs(diff)[wm].sum() / n, diff[wm].sum() / n))
        inner = stats[4][0]
        bad = [side for side, (mad, bias) in zip(('top', 'bottom', 'left', 'right'), stats[:4])
               if mad > MAD_FACTOR * inner + MAD_SLACK or abs(bias) > BIAS_MAX]
        name = e['name'].split(' ')[0]
        rows.append((name, k, t.w, t.h, stats, bad))
        if bad:
            flagged.append(name)
    print('%-40s %9s  %-13s %-13s %-13s %-13s %-6s %s' % ('texture', 'size', 'top mad/bias', 'bottom', 'left', 'right', 'inner', 'flag'))
    for name, k, w, h, stats, bad in rows:
        print('%-40s %4dx%-4d  ' % (name[:40], w, h) + ' '.join('%5.1f/%+6.1f ' % s for s in stats[:4]) + ' %5.1f  %s' % (stats[4][0], ','.join(bad)))
    worst = max(rows, key=lambda r: max(abs(b) for _, b in r[4][:4]))
    print('%d textures, %d flagged%s; largest border bias %.1f (%s)' % (len(rows), len(flagged), (': ' + ' '.join(flagged)) if flagged else '',
                                                                       max(abs(b) for _, b in worst[4][:4]), worst[0]))
    if out:
        with open(out, 'w') as f:
            f.write('texture\tkey\tw\th\ttop_mad\ttop_bias\tbottom_mad\tbottom_bias\tleft_mad\tleft_bias\tright_mad\tright_bias\tinner_mad\tflag\n')
            for name, k, w, h, stats, bad in rows:
                f.write('%s\t%016x\t%d\t%d\t%s\t%.2f\t%s\n' % (name, k, w, h, '\t'.join('%.2f\t%.2f' % s for s in stats[:4]), stats[4][0], ','.join(bad)))
    sys.exit(1 if flagged else 0)


if __name__ == '__main__':
    main()

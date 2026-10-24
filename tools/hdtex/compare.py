#!/usr/bin/env python3
"""Close-up before/after pictures from a finished pack: for each named texture, the original
(decoded from the cooked data) and the pack's level 0 (decoded from its DXT blocks), both
magnified bilinearly to the same size, as the GPU shows them in a close-up. Also checks every
index entry of the pack (offsets, level sizes, key of the original).

    python3 -E -P tools/hdtex/compare.py <pack> <out.png> [name-substring ...]
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402


def read_pack(path):
    b = open(path, 'rb').read()
    magic, ver, n, io, no, do, size, _ = struct.unpack_from('<4sIIIIIII', b, 0)
    assert magic == b'HDTX' and ver == 1 and size == len(b), 'bad header'
    out = {}
    last = -1
    for i in range(n):
        k, off, sz, w, h, ow, oh, fmt, lv, ofmt, flags, nameoff = struct.unpack_from('<QIIHHHHBBBBI', b, io + 32 * i)
        assert k > last, 'index not sorted'
        last = k
        bs = 16 if fmt == 3 else 8
        total, lw, lh = 0, w, h
        for _ in range(lv):
            total += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bs
            lw, lh = max(1, lw // 2), max(1, lh // 2)
        assert total == sz and off + sz <= len(b) and off % 64 == 0, 'entry %016x sizes' % k
        name = b[no + nameoff:b.index(b'\0', no + nameoff)].decode()
        out[k] = dict(w=w, h=h, ow=ow, oh=oh, fmt=fmt, levels=lv, data=b[off:off + max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs], name=name)
    return out


def decode_dxt(data, w, h, fmt):
    import quicktex
    import quicktex.s3tc.bc1 as bc1
    import quicktex.s3tc.bc3 as bc3
    if fmt == 1:
        tex = bc1.BC1Texture.from_bytes(data, w, h)
        raw = bc1.BC1Decoder().decode(tex)
    else:
        tex = bc3.BC3Texture.from_bytes(data, w, h)
        raw = bc3.BC3Decoder().decode(tex)
    return np.frombuffer(raw.tobytes(), np.uint8).reshape(h, w, 4)


def main():
    pack, outp, wants = sys.argv[1], sys.argv[2], sys.argv[3:]
    entries = read_pack(pack)
    texs = build_pack.collect(build_pack.DEFAULT_DATA, build_pack.read_selection(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'selection.txt')))
    missing = set(texs) ^ set(entries)
    print('%d pack entries checked; %d keys differ from the selection' % (len(entries), len(missing)))
    import gxdecode
    rows = []
    for want in wants:
        for k, e in entries.items():
            if want in e['name'].split(' ')[0]:
                t = texs[k]['t']
                orig = gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt)
                hd = decode_dxt(e['data'], e['w'], e['h'], e['fmt'])
                rows.append((e['name'].split(' ')[0], orig, hd))
                break
    S = 384
    im = Image.new('RGB', (2 * S + 12, len(rows) * (S + 16) + 16), (24, 24, 24))
    dr = ImageDraw.Draw(im)
    dr.text((4, 2), 'original (bilinear)', fill=(255, 255, 255))
    dr.text((S + 16, 2), 'HD pack level 0 (bilinear)', fill=(255, 255, 255))
    for r, (name, orig, hd) in enumerate(rows):
        sc = S / max(hd.shape[:2])
        size = (int(hd.shape[1] * sc), int(hd.shape[0] * sc))
        y = 16 + r * (S + 16)
        dr.text((4, y), name, fill=(255, 255, 0))
        for c, a in enumerate((orig, hd)):
            p = Image.fromarray(a, 'RGBA').resize(size, Image.BILINEAR)
            bg = Image.new('RGBA', size, (90, 90, 90, 255))
            im.paste(Image.alpha_composite(bg, p).convert('RGB'), (c * (S + 12), y + 12))
    im.save(outp)
    print('wrote', outp, len(rows), 'rows')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Writes the synthetic pack tests/vgx_hdtex.cpp reads, with build_pack.py's own key function and writer (so the test also checks that Python and
C++ compute the same key).
    make_test_pack.py <out.pack> [name] [--hd N] [--filler first|last]
The original: an 8x8 CMPR texture whose 32 bytes are (i * 7 + 3) & 255. The replacement: N x N (default 16) DXT1, levels down to 1x1, every byte
of level l = 0x10 + l. --filler adds a second record (a 1024x1024 DXT5 image, 1 MiB of 0xEE, never matched) whose data is written before
('first') or after ('last') the test record's, for the preload-prefix tests."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402


class T:
    fmt, w, h = 0xE, 8, 8


ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('name', nargs='?', default='Test:Synthetic')
ap.add_argument('--hd', type=int, default=16)
ap.add_argument('--filler', choices=('first', 'last'))
a = ap.parse_args()

level0 = bytes((i * 7 + 3) & 255 for i in range(32))
key = build_pack.tex_key(0xE, 8, 8, level0, b'')
levels, s, lv = [], a.hd, 0
while True:
    n = max(1, (s + 3) // 4) ** 2 * 8
    levels.append(bytes([0x10 + lv]) * n)
    if s == 1:
        break
    s //= 2
    lv += 1
hd = dict(fmt=build_pack.PACK_DXT1, w=a.hd, h=a.hd, levels=levels, opaque=True)
items = [(key, dict(names=[a.name], t=T(), prio=0 if not a.filler or a.filler == 'first' else -1), hd)]
if a.filler:
    where = a.filler
    side = 1024
    fl = [bytes([0xEE]) * (side * side)]  # DXT5, one level: 1 MiB
    fkey = key ^ 0x5555555555555555
    fhd = dict(fmt=build_pack.PACK_DXT5, w=side, h=side, levels=fl, opaque=False)
    items.append((fkey, dict(names=['Filler:Big'], t=T(), prio=-1 if where == 'first' else 1), fhd))
build_pack.write_pack(a.out, items)
print('%s: key %016x' % (a.out, key))

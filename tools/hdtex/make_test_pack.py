#!/usr/bin/env python3
"""Writes the synthetic one-texture pack tests/vgx_hdtex.cpp reads, with build_pack.py's own
key function and writer (so the test also checks that Python and C++ compute the same key).
    make_test_pack.py <out.pack>
The original: an 8x8 CMPR texture whose 32 bytes are (i * 7 + 3) & 255. The replacement:
16x16 DXT1, levels 16, 8, 4, 2, 1, every byte of level l = 0x10 + l."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402


class T:
    fmt, w, h = 0xE, 8, 8


level0 = bytes((i * 7 + 3) & 255 for i in range(32))
key = build_pack.tex_key(0xE, 8, 8, level0, b'')
levels, s = [], 16
for lv in range(5):
    n = max(1, (s + 3) // 4) ** 2 * 8
    levels.append(bytes([0x10 + lv]) * n)
    s //= 2
hd = dict(fmt=build_pack.PACK_DXT1, w=16, h=16, levels=levels, opaque=True)
build_pack.write_pack(sys.argv[1], [(key, dict(names=[sys.argv[2] if len(sys.argv) > 2 else 'Test:Synthetic'], t=T()), hd)])
print('%s: key %016x' % (sys.argv[1], key))

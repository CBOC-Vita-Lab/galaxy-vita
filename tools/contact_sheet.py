#!/usr/bin/env python3
"""Tiles RGBA8 PNG captures into one contact sheet, each scaled to a quarter
of its size (half width, half height), with the file name's position given
by the tile order.

  contact_sheet.py <out.png> <columns> <png>...
"""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
from pnginfo import read_png  # noqa: E402


def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(kind, body):
        c = struct.pack('>I', len(body)) + kind + body
        return c + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, 'wb') as fh:
        fh.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                 chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    out, cols, paths = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    tiles = []
    for p in paths:
        w, h, rows = read_png(p)
        hw, hh = w // 2, h // 2
        small = bytearray(hw * hh * 4)
        for y in range(hh):
            src = rows[2 * y]
            dst = y * hw * 4
            for c in range(3):  # every other pixel, one channel at a time
                small[dst + c:dst + hw * 4:4] = src[c:hw * 8:8]
            # Opaque: captures carry the EFB's destination alpha.
            small[dst + 3:dst + hw * 4:4] = b'\xff' * hw
        tiles.append((hw, hh, small))
    tw = max(t[0] for t in tiles)
    th = max(t[1] for t in tiles)
    rows = (len(tiles) + cols - 1) // cols
    W, H = tw * cols, th * rows
    sheet = bytearray(W * H * 4)
    for i, (w, h, px) in enumerate(tiles):
        ox, oy = (i % cols) * tw, (i // cols) * th
        for y in range(h):
            d = ((oy + y) * W + ox) * 4
            sheet[d:d + w * 4] = px[y * w * 4:(y + 1) * w * 4]
    write_png(out, W, H, sheet)
    print('%s: %dx%d, %d tiles' % (out, W, H, len(tiles)))


if __name__ == '__main__':
    main()

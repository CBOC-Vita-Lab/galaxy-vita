#!/usr/bin/env python3
"""Summarizes RGBA8 PNG captures from the headless runner and optionally
writes a half-size copy that is quicker to view.

  pnginfo.py <png>... [--half]
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h = 8, [], 0, 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype == 6, 'expects RGBA8'
        elif kind == b'IDAT':
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f != 0:
            raise ValueError('unsupported filter %d' % f)
        rows.append(line)
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(out)


def main():
    half = '--half' in sys.argv
    for path in [a for a in sys.argv[1:] if not a.startswith('--')]:
        w, h, rows = read_png(path)
        colors = {}
        for r in rows[::4]:
            for i in range(0, len(r), 16):
                c = bytes(r[i:i + 3])
                colors[c] = colors.get(c, 0) + 1
        top = sorted(colors.items(), key=lambda kv: -kv[1])[:4]
        print('%s: %dx%d, %d colors; top %s' % (path, w, h, len(colors),
              ', '.join('%s x%d' % (tuple(c), n) for c, n in top)))
        if half:
            small = [bytearray(b''.join(bytes(r[i:i + 4]) for i in range(0, len(r), 8))) for r in rows[::2]]
            write_png(path.replace('.png', '_half.png'), w // 2, h // 2, small)


if __name__ == "__main__":
    main()

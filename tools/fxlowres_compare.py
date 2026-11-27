#!/usr/bin/env python3
# GXV_FXLOWRES=1 (lowres25): compare the RGBA readbacks of tests/vgx_fxlowres_pic_gl_test (a file of [w, h][RGBA bytes] pictures).
#   fxlowres_compare.py <off.rgba> <on.rgba> [png-prefix]
# Prints, per picture: identical, or PSNR (RGB), the largest difference and the share of pixels more than 8 levels off.
# With a prefix writes <prefix>-<n>-off.png, -on.png and -diff.png (the difference x 8). No dependencies beyond the standard library.
import struct, sys, zlib, math
def load(path):
    data = open(path, 'rb').read()
    out = []; p = 0
    while p < len(data):
        w, h = struct.unpack_from('<II', data, p); p += 8
        out.append((w, h, data[p:p + w * h * 4])); p += w * h * 4
    return out
def png(path, w, h, rgba, flip=True):
    rows = [rgba[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    if flip: rows.reverse()   # (the readback's first row is the bottom one)
    raw = b''.join(b'\x00' + bytes(r[i] for i in range(len(r)) if i % 4 != 3) for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
a, b = load(sys.argv[1]), load(sys.argv[2])
prefix = sys.argv[3] if len(sys.argv) > 3 else None
assert len(a) == len(b), 'different picture counts'
for n, ((w, h, x), (w2, h2, y)) in enumerate(zip(a, b), 1):
    assert (w, h) == (w2, h2)
    if x == y:
        print('picture %d: identical' % n)
    else:
        se = 0; mx = 0; over = 0; diff = bytearray(len(x))
        for i in range(0, len(x), 4):
            worst = 0
            for c in range(3):
                d = x[i + c] - y[i + c]
                se += d * d
                ad = -d if d < 0 else d
                if ad > worst: worst = ad
                diff[i + c] = min(255, ad * 8)
            diff[i + 3] = 255
            if worst > mx: mx = worst
            if worst > 8: over += 1
        mse = se / (w * h * 3.0)
        print('picture %d: PSNR %.2f dB, largest difference %d levels, %.2f %% of pixels more than 8 levels off' % (n, 10 * math.log10(255 * 255 / mse), mx, 100.0 * over / (w * h)))
        if prefix: png('%s-%d-diff.png' % (prefix, n), w, h, bytes(diff))
    if prefix:
        png('%s-%d-off.png' % (prefix, n), w, h, x)
        png('%s-%d-on.png' % (prefix, n), w, h, y)

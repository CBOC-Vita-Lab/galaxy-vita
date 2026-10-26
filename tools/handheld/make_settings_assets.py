#!/usr/bin/env python3
"""Original bitmap console diagrams and LiveArea Settings tile, no external assets.

Uses only the Python standard library. Regeneration changes only the owned atlas
and settings.png, never the game's existing launcher artwork.
"""
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
PALETTE = [(0, 0, 0, 0), (21, 26, 32, 255), (218, 237, 241, 255),
           (57, 69, 78, 255), (34, 204, 226, 255), (248, 209, 76, 255),
           (245, 120, 183, 255), (124, 235, 174, 255), (109, 167, 250, 255),
           (250, 250, 252, 255)]


class Bitmap:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = [0] * (width * height)

    def rect(self, x, y, w, h, color):
        for py in range(max(0, y), min(self.height, y+h)):
            for px in range(max(0, x), min(self.width, x+w)):
                self.pixels[py*self.width+px] = color

    def ellipse(self, x, y, rx, ry, color):
        for py in range(max(0, y-ry), min(self.height, y+ry+1)):
            for px in range(max(0, x-rx), min(self.width, x+rx+1)):
                if ((px-x)/rx)**2 + ((py-y)/ry)**2 <= 1:
                    self.pixels[py*self.width+px] = color

    def polygon(self, points, color):
        for y in range(self.height):
            intersections = []
            for a, b in zip(points, points[1:]+points[:1]):
                if min(a[1], b[1]) <= y < max(a[1], b[1]):
                    intersections.append(a[0]+(y-a[1])*(b[0]-a[0])/(b[1]-a[1]))
            intersections.sort()
            for left, right in zip(intersections[0::2], intersections[1::2]):
                self.rect(int(left), y, int(right-left)+1, 1, color)


def buttons(im, x, y):
    for dx, dy, color in [(0, -19, 7), (19, 0, 6), (0, 19, 8), (-19, 0, 5)]:
        im.ellipse(x+dx, y+dy, 8, 8, color)
        im.ellipse(x+dx, y+dy, 5, 5, 1)
    im.rect(x-2, y+15, 4, 8, 8)
    im.rect(x-4, y+18, 8, 2, 8)
    im.polygon([(x-4, y-15), (x+4, y-15), (x, y-23)], 7)
    im.rect(x-23, y-4, 8, 8, 5)
    im.rect(x-21, y-2, 4, 4, 1)


def handheld():
    im = Bitmap(300, 150)
    im.rect(45, 19, 210, 106, 2)
    im.ellipse(45, 72, 42, 53, 2)
    im.ellipse(255, 72, 42, 53, 2)
    im.rect(45, 22, 210, 100, 1)
    im.ellipse(45, 72, 39, 50, 1)
    im.ellipse(255, 72, 39, 50, 1)
    im.rect(79, 32, 142, 82, 4)
    im.rect(83, 36, 134, 74, 3)
    im.rect(91, 43, 118, 60, 1)
    im.rect(40, 40, 10, 38, 2)
    im.rect(26, 54, 38, 10, 2)
    buttons(im, 255, 59)
    for x in [45, 255]:
        im.ellipse(x, 102, 12, 12, 3)
        im.ellipse(x, 102, 8, 8, 1)
    im.rect(22, 16, 42, 5, 4)
    im.rect(236, 16, 42, 5, 4)
    im.rect(238, 121, 11, 3, 2)
    im.rect(260, 121, 11, 3, 2)
    return im


def playstation():
    im = Bitmap(300, 150)
    im.polygon([(35, 22), (91, 13), (111, 26), (189, 26), (209, 13),
                (265, 22), (293, 127), (269, 141), (218, 98), (82, 98), (31, 141), (7, 127)], 2)
    im.polygon([(38, 26), (88, 18), (109, 31), (191, 31), (212, 18),
                (262, 26), (287, 124), (270, 134), (219, 93), (81, 93), (30, 134), (13, 124)], 1)
    im.rect(112, 35, 76, 38, 3)
    im.rect(118, 40, 64, 27, 1)
    im.rect(48, 37, 10, 40, 2)
    im.rect(33, 52, 40, 10, 2)
    buttons(im, 246, 57)
    for x in [104, 196]:
        im.ellipse(x, 93, 18, 18, 3)
        im.ellipse(x, 93, 13, 13, 1)
    im.rect(32, 10, 55, 6, 4)
    im.rect(213, 10, 55, 6, 4)
    im.rect(96, 39, 8, 17, 2)
    im.rect(196, 39, 8, 17, 2)
    return im


def star(color):
    im = Bitmap(32, 32)
    im.polygon([(16, 1), (21, 11), (31, 13), (24, 21), (26, 31),
                (16, 26), (6, 31), (8, 21), (1, 13), (11, 11)], color)
    im.polygon([(16, 1), (16, 18), (1, 13), (11, 11)], 9)
    return im


def tile():
    im = Bitmap(280, 64)
    im.rect(0, 0, 280, 64, 4)
    im.rect(2, 2, 276, 60, 1)
    for x, y, w, h in [(23, 14, 7, 36), (17, 20, 20, 24), (11, 26, 32, 12)]:
        im.rect(x, y, w, h, 9)
    im.ellipse(27, 32, 10, 10, 9)
    im.ellipse(27, 32, 5, 5, 1)
    glyphs = {'S': ['11111', '10000', '10000', '11111', '00001', '00001', '11111'],
              'E': ['11111', '10000', '10000', '11110', '10000', '10000', '11111'],
              'T': ['11111', '00100', '00100', '00100', '00100', '00100', '00100'],
              'I': ['11111', '00100', '00100', '00100', '00100', '00100', '11111'],
              'N': ['10001', '11001', '11001', '10101', '10011', '10011', '10001'],
              'G': ['11111', '10000', '10000', '10111', '10001', '10001', '11111']}
    for index, ch in enumerate('SETTINGS'):
        for y, row in enumerate(glyphs[ch]):
            for x, pixel in enumerate(row):
                if pixel == '1':
                    im.rect(62+index*25+x*4, 18+y*4, 4, 4, 9)
    return im


def symbol(kind):
    im = Bitmap(28, 28)
    color = {'Cross': 8, 'Circle': 6, 'Square': 5, 'Triangle': 7}.get(kind, 2)
    im.ellipse(14, 14, 13, 13, 3)
    im.ellipse(14, 14, 11, 11, 1)
    if kind == 'Cross':
        for at in range(8, 21):
            im.rect(at, at-1, 2, 3, color)
            im.rect(28-at, at-1, 2, 3, color)
    elif kind == 'Circle':
        im.ellipse(14, 14, 8, 8, color)
        im.ellipse(14, 14, 5, 5, 1)
    elif kind == 'Square':
        im.rect(7, 7, 15, 15, color)
        im.rect(10, 10, 9, 9, 1)
    elif kind == 'Triangle':
        im.polygon([(14, 5), (24, 22), (4, 22)], color)
        im.polygon([(14, 11), (19, 19), (9, 19)], 1)
    elif kind == 'Start':
        im.polygon([(9, 6), (22, 14), (9, 22)], color)
    else:
        im.rect(7, 8, 14, 12, color)
        im.rect(10, 11, 8, 6, 1)
    return im


def chunk(kind, data):
    return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data) & 0xffffffff)


def rgba_png(path, expected=(300, 129)):
    """Decode the owned 8-bit RGBA PNG for lossless C++ embedding, not image editing."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    at, packed = 8, bytearray()
    width = height = 0
    while at < len(data):
        count = struct.unpack_from('>I', data, at)[0]
        kind, payload = data[at+4:at+8], data[at+8:at+8+count]
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            assert (depth, color, compression, filtering, interlace) == (8, 6, 0, 0, 0)
        elif kind == b'IDAT':
            packed.extend(payload)
        at += count+12
    assert (width, height) == expected
    raw = zlib.decompress(packed)
    stride, pixels, previous = width*4, bytearray(), bytearray(width*4)
    assert len(raw) == height*(stride+1)
    for y in range(height):
        method = raw[y*(stride+1)]
        row = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        assert method < 5
        for x in range(stride):
            left, above, diagonal = row[x-4] if x >= 4 else 0, previous[x], previous[x-4] if x >= 4 else 0
            if method == 1:
                prediction = left
            elif method == 2:
                prediction = above
            elif method == 3:
                prediction = (left+above)//2
            elif method == 4:
                base = left+above-diagonal
                distances = abs(base-left), abs(base-above), abs(base-diagonal)
                prediction = (left, above, diagonal)[distances.index(min(distances))]
            else:
                prediction = 0
            row[x] = (row[x]+prediction) & 255
        pixels.extend(row)
        previous = row
    return pixels


def png(im, path):
    data = b''.join(b'\0'+bytes(im.pixels[y*im.width:(y+1)*im.width]) for y in range(im.height))
    rgb = bytes(v for color in PALETTE for v in color[:3])
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', im.width, im.height, 8, 3, 0, 0, 0))+
                     chunk(b'PLTE', rgb)+chunk(b'IDAT', zlib.compress(data, 9))+chunk(b'IEND', b''))


def main():
    assets = [('Handheld', handheld()), ('PlayStation', playstation()),
              ('StarGold', star(5)), ('StarMint', star(7)), ('StarPink', star(6))]
    assets.extend(('Button'+kind, symbol(kind)) for kind in ['Cross', 'Circle', 'Square', 'Triangle', 'Start', 'Select'])
    lines = ['// Generated original bitmap art by tools/handheld/make_settings_assets.py.',
             'struct AssetRun { unsigned short count; unsigned char color; };',
             'static constexpr uint32_t kAssetPalette[] = {']
    lines.append(','.join('0x%08xu' % (a << 24 | b << 16 | g << 8 | r) for r, g, b, a in PALETTE)+'};')
    for name, im in assets:
        runs = []
        for pixel in im.pixels:
            if runs and runs[-1][1] == pixel and runs[-1][0] < 65535:
                runs[-1][0] += 1
            else:
                runs.append([1, pixel])
        lines.append('static constexpr AssetRun k%s[] = {' % name)
        lines.extend('    '+','.join('{%d,%d}' % tuple(run) for run in runs[i:i+12])+',' for i in range(0, len(runs), 12))
        lines.append('};')
    (ROOT/'platform/src/settings/settings_assets.inc').write_text('\n'.join(lines)+'\n')
    for filename, name, size in [('vita', 'Vita', (300, 129)), ('playstation', 'PlayStation', (250, 167))]:
        rgba = rgba_png(pathlib.Path(__file__).resolve().parent/f'assets/settings_{filename}.png', size)
        runs = []
        for red, green, blue, alpha in zip(rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4]):
            pixel = alpha << 24 | blue << 16 | green << 8 | red
            if runs and runs[-1][1] == pixel and runs[-1][0] < 65535:
                runs[-1][0] += 1
            else:
                runs.append([1, pixel])
        real = [f'// Original generated console illustration, lossless {size[0]} x {size[1]} RGBA.',
                '// Encoded by tools/handheld/make_settings_assets.py from the owned PNG.']
        if filename == 'vita':
            real.append('struct RgbaRun { unsigned short count; uint32_t pixel; };')
        real.append(f'static constexpr RgbaRun k{name}Illustration[] = {{')
        real.extend('    '+','.join('{%d,0x%08xu}' % tuple(run) for run in runs[i:i+8])+',' for i in range(0, len(runs), 8))
        real.append('};')
        (ROOT/f'platform/src/settings/settings_{filename}.inc').write_text('\n'.join(real)+'\n')
    png(tile(), ROOT/'vita/vgx-game/sce_sys/livearea/contents/settings.png')


if __name__ == '__main__':
    main()

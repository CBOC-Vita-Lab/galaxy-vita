#!/usr/bin/env python3
"""Checks the package's bubble and LiveArea pictures and template before they go into the VPK (a picture of the wrong kind makes
the console refuse to install the package, or shows an empty LiveArea).
Each picture must be: a valid PNG (signature, chunk CRCs, a zlib stream that inflates to exactly height * (width + 1) bytes with
valid filter bytes), of its fixed size, 8-bit palette (colour type 3) with no interlace, chunks IHDR, PLTE (256 entries), IDAT, IEND
and nothing else (no text or time chunks), every index inside the palette, and under the size cap below. Transparency: the gate's
startup.png may carry a tRNS chunk (palette alpha, after PLTE, at most one entry a palette colour): the LiveArea gate image supports
alpha, and the approved contributor startup.png (vita/vgx-game/sce_sys/ARTWORK.md) has one (fix31; it was refused before, so this check
was red on the art the package ships). The bubble icon, pic0 and the background stay opaque (no tRNS): the icon must not have alpha.
With --like <dir> the same properties are also compared with the pictures in <dir> (for example the files of the last release).
template.xml must parse, keep the a1 style, the background and the gate, must NOT carry the old Settings tile (frame3, settings.png, psla:-settings: Settings live in the
game now), and name only pictures that exist.
  python3 tools/handheld/check_livearea.py [sce_sys-dir] [--like <dir>]      (exit 1 on any failure)"""
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# name: (width, height, size cap in bytes). The caps are this project's own, deliberately well under anything reported for the
# console (no official figure is at hand); the generator's pictures are about a third of them.
PICTURES = {
    'icon0.png': (128, 128, 64 * 1024),
    'pic0.png': (960, 544, 512 * 1024),
    'livearea/contents/bg.png': (840, 500, 512 * 1024),
    'livearea/contents/startup.png': (280, 158, 128 * 1024),
}
ALLOWED = [b'IHDR', b'PLTE', b'IDAT', b'IEND']
TRNS_OK = {'livearea/contents/startup.png'}   # (fix31: the gate image supports alpha; the others are opaque)


def chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG signature')
    pos, out = 8, []
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError('truncated chunk')
        n, = struct.unpack('>I', data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        crc, = struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])
        if len(body) != n or zlib.crc32(kind + body) & 0xffffffff != crc:
            raise ValueError(f'bad chunk {kind!r} (length or CRC)')
        out.append((kind, body))
        pos += 12 + n
        if kind == b'IEND':
            break
    if pos != len(data):
        raise ValueError('bytes after IEND')
    return out


def properties(path):
    data = open(path, 'rb').read()
    cs = chunks(data)
    kinds = [k for k, _ in cs]
    ihdr = struct.unpack('>IIBBBBB', cs[0][1]) if kinds and kinds[0] == b'IHDR' else None
    plte = dict(cs).get(b'PLTE', b'')
    return {'size': len(data), 'ihdr': ihdr, 'kinds': sorted(set(kinds)), 'order': [k for k in kinds if k != b'IDAT'] , 'plte': len(plte),
            'chunks': cs}


def check_picture(path, w, h, cap, trns_ok=False):
    errors = []
    p = properties(path)
    if p['ihdr'] is None:
        return ['IHDR is not the first chunk']
    W, H, depth, ctype, comp, filt, inter = p['ihdr']
    if (W, H) != (w, h):
        errors.append(f'size {W}x{H}, wanted {w}x{h}')
    if (depth, ctype) != (8, 3):
        errors.append(f'bit depth {depth} colour type {ctype}: wanted an 8-bit palette (8, 3)')
    if (comp, filt, inter) != (0, 0, 0):
        errors.append(f'compression {comp} filter {filt} interlace {inter}: wanted 0 0 0 (no interlace)')
    allowed = ALLOWED + ([b'tRNS'] if trns_ok else [])
    extra = [k for k in p['kinds'] if k not in allowed]
    if extra:
        errors.append(f'extra chunks {extra} (no text / time chunks; tRNS only in startup.png)')
    if p['order'] not in ([b'IHDR', b'PLTE', b'IEND'], [b'IHDR', b'PLTE', b'tRNS', b'IEND'] if trns_ok else None):
        errors.append(f'chunk order {p["order"]}')
    trns = dict(p['chunks']).get(b'tRNS')
    if trns is not None and trns_ok and len(trns) > p['plte'] // 3:
        errors.append(f'tRNS has {len(trns)} entries, more than the {p["plte"] // 3} palette colours')
    if p['plte'] != 768:
        errors.append(f'palette of {p["plte"] // 3} entries, wanted 256')
    if p['size'] > cap:
        errors.append(f'{p["size"]} bytes, over the cap of {cap}')
    try:
        raw = zlib.decompress(b''.join(b for k, b in p['chunks'] if k == b'IDAT'))
    except zlib.error as e:
        return errors + [f'IDAT does not inflate: {e}']
    if len(raw) != H * (W + 1):
        errors.append(f'image data is {len(raw)} bytes, wanted {H * (W + 1)}')
    elif any(raw[y * (W + 1)] > 4 for y in range(H)):
        errors.append('a row has an invalid filter type')
    elif all(raw[y * (W + 1)] == 0 for y in range(H)):  # unfiltered rows hold the indices themselves
        top = max(max(raw[y * (W + 1) + 1:(y + 1) * (W + 1)]) for y in range(H))
        if top >= p['plte'] // 3:
            errors.append(f'index {top} outside the palette')
    return errors


def check_template(folder):
    errors = []
    path = os.path.join(folder, 'livearea', 'contents', 'template.xml')
    try:
        root = ET.parse(path).getroot()
    except Exception as e:  # noqa: BLE001
        return [f'template.xml does not parse: {e}']
    if root.tag != 'livearea' or root.get('style') != 'a1' or root.get('format-ver') != '01.00' or not (root.get('content-rev') or '').isdigit():
        errors.append('template.xml: <livearea style="a1" format-ver="01.00" content-rev="<number>"> expected')
    if root.findtext('livearea-background/image') != 'bg.png':
        errors.append('template.xml: the background must be bg.png')
    if root.findtext('gate/startup-image') != 'startup.png':
        errors.append('template.xml: the gate must show startup.png')
    if [f for f in root.iter('frame') if f.findtext('liveitem/target') == 'psla:-settings' or f.get('id') == 'frame3']:
        errors.append('template.xml: the Settings tile (frame3, psla:-settings) must be absent (Settings are in the game)')
    if root.find('.//liveitem') is not None or any((i.text or '') == 'settings.png' for i in root.iter('image')):
        errors.append('template.xml: no liveitem and no settings.png may be named')
    if os.path.isfile(os.path.join(folder, 'livearea', 'contents', 'settings.png')) and 'settings.png' in open(path).read():
        errors.append('template.xml: settings.png must not be named')
    for image in root.iter('image'):
        if not os.path.isfile(os.path.join(folder, 'livearea', 'contents', image.text or '')):
            errors.append(f'template.xml names {image.text}, which is not there')
    for image in root.iter('startup-image'):
        if not os.path.isfile(os.path.join(folder, 'livearea', 'contents', image.text or '')):
            errors.append(f'template.xml names {image.text}, which is not there')
    return errors


def main(argv):
    folder = os.path.join(HERE, '..', '..', 'vita', 'vgx-game', 'sce_sys')
    like = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == '--like':
            like = args.pop(0)
        else:
            folder = a
    failed = False
    for name, (w, h, cap) in PICTURES.items():
        path = os.path.join(folder, name)
        try:
            errors = check_picture(path, w, h, cap, name in TRNS_OK)
            if like:
                mine, ref = properties(path), properties(os.path.join(like, name))
                for key in ('kinds', 'order', 'plte'):
                    if mine[key] != ref[key]:
                        errors.append(f'{key} differs from {like}: {mine[key]} vs {ref[key]}')
                if mine['ihdr'] != ref['ihdr']:
                    errors.append(f'IHDR differs from {like}: {mine["ihdr"]} vs {ref["ihdr"]}')
        except (OSError, ValueError, struct.error) as e:
            errors = [str(e)]
        size = os.path.getsize(path) if os.path.isfile(path) else 0
        try:
            alpha = ' (palette alpha)' if name in TRNS_OK and b'tRNS' in properties(path)['kinds'] else ''
        except (OSError, ValueError, struct.error):
            alpha = ''
        print(f'{"FAIL" if errors else "ok  "} {name:32s} {w}x{h} 8-bit palette{alpha}, {size} bytes (cap {cap})' + ''.join(f'\n     {e}' for e in errors))
        failed = failed or bool(errors)
    errors = check_template(folder)
    print(f'{"FAIL" if errors else "ok  "} livearea/contents/template.xml' + ''.join(f'\n     {e}' for e in errors))
    failed = failed or bool(errors)
    print('FAILED' if failed else 'PASS: all pictures and the template are of the kind the console installs')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

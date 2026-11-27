#!/usr/bin/env python3
"""Build the Galaxy Vita LiveArea and bubble pictures from YOUR OWN Super Mario Galaxy files.

The public Galaxy-Vita.vpk carries plain generic star art, because Nintendo's art can never be
shipped.  This tool runs on your computer, reads the title logo out of the game files you extracted
from your own disc, puts it on a procedurally drawn starfield, and writes:

    sce_sys/icon0.png                          128 x 128   the bubble
    sce_sys/livearea/contents/bg.png           840 x 500   LiveArea background
    sce_sys/livearea/contents/startup.png      280 x 158   LiveArea start button
    sce_sys/pic0.png                           960 x 544   (optional, --pic0)

All of them are 8-bit palette PNGs, which the Vita package installer requires for icon0 and bg.
Nothing is downloaded, and the pictures are made only on your machine from your files.  Do not
share the result: it contains Nintendo's logo.

    python3 tools/vita/livearea_art.py /path/to/your/game/files --check
    python3 tools/vita/livearea_art.py /path/to/your/game/files --png-dir art
    python3 tools/vita/livearea_art.py /path/to/your/game/files --vpk Galaxy-Vita.vpk --out Galaxy-Vita-art.vpk

The approach (read the game's own logo texture, compose with a procedural background, quantise to
palette PNGs, rewrite the VPK's sce_sys entries) follows danos-x5/NFSUG2_VITA's
tools/vita/livearea_art.py.  This file is an independent implementation; no code was copied.
(That project is MIT licensed.)

Needs Python 3 and Pillow (pip install pillow).
"""
import argparse
import os
import random
import shutil
import struct
import sys
import tempfile
import zipfile

SIZES = {
    'sce_sys/icon0.png': (128, 128),
    'sce_sys/livearea/contents/bg.png': (840, 500),
    'sce_sys/livearea/contents/startup.png': (280, 158),
    'sce_sys/pic0.png': (960, 544),
}
ICON, BG, STARTUP, PIC0 = list(SIZES)

LOGO_ARCHIVE = 'TitleLogo.arc'
LOGO_TEXTURE = 'mytitlelogo.tpl'

NEED_MSG = ('this needs your own Super Mario Galaxy files: ')


class ArtError(Exception):
    pass


# ---------------------------------------------------------------------------
# Yaz0 and RARC (original big-endian archives, and the port's cooked little-endian "CRAR")
# ---------------------------------------------------------------------------
def yaz0_decompress(data):
    if data[:4] != b'Yaz0':
        return data
    size = struct.unpack_from('>I', data, 4)[0]
    out = bytearray(size)
    src, dst, n = 16, 0, len(data)
    try:
        while dst < size:
            code = data[src]
            src += 1
            for bit in range(8):
                if dst >= size:
                    break
                if code & (0x80 >> bit):
                    out[dst] = data[src]
                    dst += 1
                    src += 1
                else:
                    b1, b2 = data[src], data[src + 1]
                    src += 2
                    cnt = b1 >> 4
                    if cnt == 0:
                        cnt = data[src] + 0x12
                        src += 1
                    else:
                        cnt += 2
                    cp = dst - (((b1 & 0x0F) << 8) | b2) - 1
                    if cp < 0:
                        raise ArtError('corrupt Yaz0 data')
                    for _ in range(cnt):
                        if dst >= size:
                            break
                        out[dst] = out[cp]
                        dst += 1
                        cp += 1
    except IndexError:
        raise ArtError('corrupt Yaz0 data (truncated)')
    return bytes(out)


def rarc_files(data):
    """Returns [(path, bytes)] for a RARC ('RARC', big-endian) or cooked ('CRAR', little-endian) archive."""
    data = yaz0_decompress(data)
    if data[:4] == b'RARC':
        e = '>'
    elif data[:4] == b'CRAR':
        e = '<'
    else:
        raise ArtError('not a RARC archive')

    def u16(o): return struct.unpack_from(e + 'H', data, o)[0]
    def u32(o): return struct.unpack_from(e + 'I', data, o)[0]

    try:
        hdr = u32(8)
        data_start = hdr + u32(0xC)
        ndirs, dir_off, nfiles, file_off, _, str_off = (u32(hdr + 4 * i) for i in range(6))
        dir_off += hdr
        file_off += hdr
        str_off += hdr

        def cstr(o):
            end = data.index(b'\0', str_off + o)
            return data[str_off + o:end].decode('shift_jis', errors='replace')

        dirs = [(cstr(u32(dir_off + 16 * i + 4)), u16(dir_off + 16 * i + 0xA), u32(dir_off + 16 * i + 0xC))
                for i in range(ndirs)]
        out = []

        def walk(di, prefix, depth):
            if depth > 32:
                return
            name, count, first = dirs[di]
            for i in range(first, first + count):
                f = file_off + 0x14 * i
                # original: u16 type/flags, u16 name offset; cooked copy: the whole 32-bit word is byte-reversed
                if e == '>':
                    flags, nameoff = u16(f + 4) >> 8, u16(f + 6)
                else:
                    flags, nameoff = u16(f + 6) >> 8, u16(f + 4)
                ename = cstr(nameoff)
                if flags & 0x02:
                    if ename not in ('.', '..'):
                        walk(u32(f + 8), prefix + ename + '/', depth + 1)
                else:
                    o = data_start + u32(f + 8)
                    out.append((prefix + ename, data[o:o + u32(f + 0xC)]))

        walk(0, dirs[0][0] + '/', 0)
        return out
    except (struct.error, IndexError, ValueError):
        raise ArtError('damaged RARC archive')


# ---------------------------------------------------------------------------
# GX textures (TPL) -> RGBA
# ---------------------------------------------------------------------------
# format id: (block width, block height, bits per texel, name)
GX_FORMATS = {
    0x0: (8, 8, 4, 'I4'), 0x1: (8, 4, 8, 'I8'), 0x2: (8, 4, 8, 'IA4'), 0x3: (4, 4, 16, 'IA8'),
    0x4: (4, 4, 16, 'RGB565'), 0x5: (4, 4, 16, 'RGB5A3'), 0x6: (4, 4, 32, 'RGBA8'),
    0x8: (8, 8, 4, 'C4'), 0x9: (8, 4, 8, 'C8'), 0xA: (4, 4, 16, 'C14X2'), 0xE: (8, 8, 4, 'CMPR'),
}
MAX_TEX = 2048


def _e5(v): return (v << 3) | (v >> 2)
def _e6(v): return (v << 2) | (v >> 4)
def _e4(v): return v * 17
def _e3(v): return (v << 5) | (v << 2) | (v >> 1)


def _rgb565(v):
    return (_e5(v >> 11), _e6((v >> 5) & 63), _e5(v & 31), 255)


def _rgb5a3(v):
    if v & 0x8000:
        return (_e5((v >> 10) & 31), _e5((v >> 5) & 31), _e5(v & 31), 255)
    return (_e4((v >> 8) & 15), _e4((v >> 4) & 15), _e4(v & 15), _e3((v >> 12) & 7))


def _ia8(v):
    i = v & 255
    return (i, i, i, v >> 8)


def decode_palette(raw, fmt):
    """fmt: 0 IA8, 1 RGB565, 2 RGB5A3.  raw is big-endian u16 entries."""
    conv = {0: _ia8, 1: _rgb565, 2: _rgb5a3}.get(fmt)
    if conv is None:
        raise ArtError('unsupported palette format %d' % fmt)
    return [conv(struct.unpack_from('>H', raw, i)[0]) for i in range(0, len(raw) - 1, 2)]


def decode_gx(fmt, w, h, data, palette=None):
    """Decodes level 0 of a GX texture to RGBA bytes (w*h*4).  `palette` is a list of RGBA tuples for C4/C8/C14X2."""
    if fmt not in GX_FORMATS:
        raise ArtError('unsupported GX texture format %d' % fmt)
    if not (0 < w <= MAX_TEX and 0 < h <= MAX_TEX):
        raise ArtError('implausible texture size %dx%d' % (w, h))
    bw, bh, bpp, name = GX_FORMATS[fmt]
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    need = bx * by * bw * bh * bpp // 8
    if len(data) < need:
        raise ArtError('texture data is short (%d of %d bytes)' % (len(data), need))
    if name in ('C4', 'C8', 'C14X2') and not palette:
        raise ArtError('palette texture without a palette')
    out = bytearray(w * h * 4)
    pos = 0

    def put(x, y, px):
        if x < w and y < h:
            o = (y * w + x) * 4
            out[o:o + 4] = bytes(px)

    for ty in range(by):
        for tx in range(bx):
            x0, y0 = tx * bw, ty * bh
            if name == 'CMPR':
                for sub in range(4):
                    sx, sy = x0 + (sub & 1) * 4, y0 + (sub >> 1) * 4
                    c0, c1 = struct.unpack_from('>HH', data, pos)
                    bits = data[pos + 4:pos + 8]
                    pos += 8
                    p0, p1 = _rgb565(c0), _rgb565(c1)
                    if c0 > c1:
                        cols = [p0, p1,
                                tuple((2 * a + b) // 3 for a, b in zip(p0[:3], p1[:3])) + (255,),
                                tuple((a + 2 * b) // 3 for a, b in zip(p0[:3], p1[:3])) + (255,)]
                    else:
                        cols = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0[:3], p1[:3])) + (255,), (0, 0, 0, 0)]
                    for r in range(4):
                        row = bits[r]
                        for c in range(4):
                            put(sx + c, sy + r, cols[(row >> (6 - 2 * c)) & 3])
            elif name == 'RGBA8':
                blk = data[pos:pos + 64]
                pos += 64
                for i in range(16):
                    a, r = blk[2 * i], blk[2 * i + 1]
                    g, b = blk[32 + 2 * i], blk[33 + 2 * i]
                    put(x0 + (i & 3), y0 + (i >> 2), (r, g, b, a))
            elif bpp == 4:
                for r in range(bh):
                    for c in range(0, bw, 2):
                        v = data[pos]
                        pos += 1
                        for k, n in enumerate((v >> 4, v & 15)):
                            if name == 'I4':
                                px = (_e4(n),) * 4
                            else:
                                px = palette[n] if n < len(palette) else (0, 0, 0, 0)
                            put(x0 + c + k, y0 + r, px)
            elif bpp == 8:
                for r in range(bh):
                    for c in range(bw):
                        v = data[pos]
                        pos += 1
                        if name == 'I8':
                            px = (v, v, v, v)           # like the GPU: intensity fills alpha too
                        elif name == 'IA4':
                            i = _e4(v & 15)
                            px = (i, i, i, _e4(v >> 4))
                        else:
                            px = palette[v] if v < len(palette) else (0, 0, 0, 0)
                        put(x0 + c, y0 + r, px)
            else:  # 16 bit
                for r in range(bh):
                    for c in range(bw):
                        v = struct.unpack_from('>H', data, pos)[0]
                        pos += 2
                        if name == 'IA8':
                            px = _ia8(v)
                        elif name == 'RGB565':
                            px = _rgb565(v)
                        elif name == 'RGB5A3':
                            px = _rgb5a3(v)
                        else:  # C14X2
                            n = v & 0x3FFF
                            px = palette[n] if n < len(palette) else (0, 0, 0, 0)
                        put(x0 + c, y0 + r, px)
    return bytes(out)


def parse_tpl(data):
    """Returns (width, height, RGBA bytes, format name) of image 0 of a TPL texture palette."""
    if data[:4] == b'\x00\x20\xaf\x30':
        e = '>'
    elif data[:4] == b'\x30\xaf\x20\x00':     # the port's cooked copy: headers little-endian, texels still big-endian
        e = '<'
    else:
        raise ArtError('not a TPL texture')
    try:
        count, table = struct.unpack_from(e + 'II', data, 4)
        if count < 1:
            raise ArtError('empty TPL')
        img, pal = struct.unpack_from(e + 'II', data, table)
        h, w, fmt, off = struct.unpack_from(e + 'HHII', data, img)
        palette = None
        if pal:
            n, _, pfmt, poff = struct.unpack_from(e + 'HHII', data, pal)
            palette = decode_palette(data[poff:poff + n * 2], pfmt)
        if fmt not in GX_FORMATS:
            raise ArtError('unsupported GX texture format %d' % fmt)
        bw, bh, bpp, name = GX_FORMATS[fmt]
        if not (0 < w <= MAX_TEX and 0 < h <= MAX_TEX):
            raise ArtError('implausible texture size %dx%d' % (w, h))
        size = ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8
        return w, h, decode_gx(fmt, w, h, data[off:off + size], palette), name
    except struct.error:
        raise ArtError('damaged TPL texture')


# ---------------------------------------------------------------------------
# finding the player's files
# ---------------------------------------------------------------------------
def find_layout_dir(root):
    cands = []
    for sub in ('', 'files', 'DATA/files', 'data/files', 'game', 'game/files'):
        cands.append(os.path.join(root, sub, 'LayoutData'))
    try:
        for d in sorted(os.listdir(root)):
            p = os.path.join(root, d)
            if os.path.isdir(p):
                cands.append(os.path.join(p, 'LayoutData'))
                cands.append(os.path.join(p, 'files', 'LayoutData'))
    except OSError:
        pass
    for c in cands:
        if os.path.isfile(os.path.join(c, LOGO_ARCHIVE)):
            return c
    return None


def load_logo(root):
    """Returns (info dict, RGBA PIL image of the logo, cropped to its visible part)."""
    from PIL import Image
    if not os.path.isdir(root):
        raise ArtError(NEED_MSG + '"%s" is not a folder. Give the folder with LayoutData, ObjectData, ... in it.' % root)
    layout = find_layout_dir(root)
    if layout is None:
        raise ArtError(NEED_MSG + 'no LayoutData/%s under "%s". Give the folder that holds the game archives '
                       '(LayoutData, ObjectData, StageData, ...).' % (LOGO_ARCHIVE, root))
    arc = os.path.join(layout, LOGO_ARCHIVE)
    with open(arc, 'rb') as f:
        files = rarc_files(f.read())
    tex = [(p, b) for p, b in files if p.lower().endswith('/' + LOGO_TEXTURE)]
    if not tex:
        names = ', '.join(p.rsplit('/', 1)[-1] for p, _ in files if p.lower().endswith('.tpl'))
        raise ArtError(NEED_MSG + '%s has no %s (found: %s).' % (arc, LOGO_TEXTURE, names or 'no textures'))
    path, blob = tex[0]
    w, h, rgba, fname = parse_tpl(blob)
    img = Image.frombytes('RGBA', (w, h), rgba)
    box = img.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox()
    if box is None:
        raise ArtError('the logo texture in %s is empty' % arc)
    info = {'archive': arc, 'texture': path, 'size': (w, h), 'format': fname, 'visible': box}
    return info, img.crop(box)


# ---------------------------------------------------------------------------
# composing
# ---------------------------------------------------------------------------
def background(size):
    """The dark blue radial glow, without stars."""
    from PIL import Image
    w, h = size
    glow = Image.radial_gradient('L').resize((w, h), Image.BILINEAR)   # 0 in the centre, 255 at the corners
    inner, outer = (22, 28, 74), (3, 4, 14)
    return Image.composite(Image.new('RGB', size, outer), Image.new('RGB', size, inner), glow.point(lambda v: min(255, v * 5 // 4)))


def starfield(size, seed, avoid=None):
    """Dark blue radial glow with scattered stars.  Entirely procedural and deterministic for a seed.
    `avoid` is an optional "L" mask the size of the picture: no star or sparkle touches a non-zero pixel of it."""
    from PIL import Image, ImageDraw, ImageFilter
    w, h = size
    rng = random.Random(seed)
    base = background(size)

    def blocked(x, y, reach):
        if avoid is None:
            return False
        box = (max(0, x - reach - 2), max(0, y - reach - 2), min(w, x + reach + 3), min(h, y + reach + 3))
        return avoid.crop(box).getextrema()[1] > 0

    stars = Image.new('RGB', size, (0, 0, 0))
    d = ImageDraw.Draw(stars)
    scale = max(1.0, w / 560.0)
    for _ in range(max(40, w * h // 1800)):
        x, y = rng.randrange(w), rng.randrange(h)
        b = rng.choice((90, 120, 160, 200, 255))
        tint = rng.choice(((1, 1, 1), (1, 1, 1), (0.8, 0.9, 1), (1, 0.95, 0.75)))
        c = tuple(int(b * t) for t in tint)
        r = 0 if rng.random() < 0.85 else int(round(scale * rng.choice((1, 1, 2))))
        if not blocked(x, y, r):          # the random draws above happen either way, so the other stars do not move
            d.ellipse((x - r, y - r, x + r, y + r), fill=c)
    for _ in range(max(2, w * h // 40000)):          # a few bigger four-point sparkles
        x, y = rng.randrange(w), rng.randrange(h)
        arm = int(rng.choice((3, 4, 5)) * max(1.0, scale * 1.5))
        if blocked(x, y, arm):
            continue
        d.line((x - arm, y, x + arm, y), fill=(200, 210, 255))
        d.line((x, y - arm, x, y + arm), fill=(200, 210, 255))
        d.point((x, y), fill=(255, 255, 255))
    soft = stars.filter(ImageFilter.GaussianBlur(0.6 * scale / 2))
    from PIL import ImageChops
    return ImageChops.add(base, ImageChops.add(stars, soft))


def logo_layout(logo, size, fill, max_h=0.8):
    """Scales the logo to `fill` of the width (at most `max_h` of the height), centred.
    Returns (scaled logo, position, "L" mask of the picture size: the logo's visible pixels plus a margin)."""
    from PIL import Image, ImageFilter
    w, h = size
    lw, lh = logo.size
    scale = min(w * fill / lw, h * max_h / lh)
    nw, nh = max(1, round(lw * scale)), max(1, round(lh * scale))
    small = logo.resize((nw, nh), Image.LANCZOS)
    pos = ((w - nw) // 2, (h - nh) // 2)
    mask = Image.new('L', size, 0)
    mask.paste(small.getchannel('A').point(lambda a: 255 if a > 8 else 0), pos)
    margin = max(3, w // 70)
    return small, pos, mask.filter(ImageFilter.MaxFilter(2 * margin + 1))


def compose(logo, size, fill, seed, max_h=0.8):
    """Places the logo, scaled to `fill` of the width (and at most `max_h` of the height), on a starfield
    that keeps its stars and sparkles off the logo."""
    from PIL import Image, ImageFilter
    w, h = size
    small, pos, avoid = logo_layout(logo, size, fill, max_h)
    nw, nh = small.size
    canvas = starfield(size, seed, avoid).convert('RGBA')
    # soft blue halo so the dark letters read against the starfield
    pad = max(3, nh // 6)
    halo = Image.new('RGBA', (nw + 2 * pad, nh + 2 * pad), (0, 0, 0, 0))
    halo.paste((70, 95, 190, 170), (pad, pad), small.getchannel('A'))
    halo = halo.filter(ImageFilter.GaussianBlur(max(1, pad // 2)))
    hx, hy = pos[0] - pad, pos[1] - pad
    cx, cy = max(0, -hx), max(0, -hy)
    canvas.alpha_composite(halo.crop((cx, cy, halo.width, halo.height)), (hx + cx, hy + cy))
    canvas.alpha_composite(small, pos)
    return canvas.convert('RGB')


def to_palette(img, colors=256):
    """Median-cut quantise an RGB image to an 8-bit palette image."""
    from PIL import Image
    return img.convert('RGB').quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE)


def png_bytes(pimg):
    import io
    buf = io.BytesIO()
    pimg.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def check_png(blob, want_size, label='picture'):
    """Raises unless blob is a PNG of the wanted size, 8-bit, colour type 3 (palette)."""
    if blob[:8] != b'\x89PNG\r\n\x1a\n' or blob[12:16] != b'IHDR':
        raise ArtError('%s is not a PNG' % label)
    w, h, depth, ctype = struct.unpack_from('>IIBB', blob, 16)
    if (w, h) != tuple(want_size):
        raise ArtError('%s is %dx%d, wanted %dx%d' % (label, w, h, want_size[0], want_size[1]))
    if depth != 8 or ctype != 3:
        raise ArtError('%s is not an 8-bit palette PNG (depth %d, colour type %d)' % (label, depth, ctype))
    if b'PLTE' not in blob[:4096]:
        raise ArtError('%s has no palette' % label)


def build_art(logo, pic0=False):
    """{vpk path: PNG bytes} for every picture, all validated."""
    spec = [(ICON, 0.95, 11), (BG, 0.56, 22), (STARTUP, 0.88, 33)]
    if pic0:
        spec.append((PIC0, 0.56, 44))
    art = {}
    for path, fill, seed in spec:
        blob = png_bytes(to_palette(compose(logo, SIZES[path], fill, seed)))
        check_png(blob, SIZES[path], path)
        art[path] = blob
    return art


# ---------------------------------------------------------------------------
# VPK
# ---------------------------------------------------------------------------
def patch_vpk(src, dst, art):
    """Writes a copy of the VPK `src` to `dst` with the pictures of `art` replaced; `src` is never modified.
    Every other entry keeps its bytes, name, order, compression and timestamps."""
    if os.path.abspath(src) == os.path.abspath(dst) or (os.path.exists(dst) and os.path.samefile(src, dst)):
        raise ArtError('--out must be a different file from the input VPK (the input is never modified)')
    try:
        zin = zipfile.ZipFile(src)
    except (zipfile.BadZipFile, OSError) as ex:
        raise ArtError('"%s" is not a readable VPK (zip) file: %s' % (src, ex))
    with zin:
        names = zin.namelist()
        for need in ('eboot.bin', 'sce_sys/livearea/contents/template.xml'):
            if need not in names:
                raise ArtError('"%s" does not look like the Galaxy Vita VPK: %s is missing' % (src, need))
        fd, tmp = tempfile.mkstemp(suffix='.vpk', dir=os.path.dirname(os.path.abspath(dst)))
        os.close(fd)
        try:
            with zipfile.ZipFile(tmp, 'w') as zout:
                done = set()
                for info in zin.infolist():
                    if info.filename in art:
                        ni = zipfile.ZipInfo(info.filename, info.date_time)
                        ni.compress_type = zipfile.ZIP_DEFLATED
                        ni.external_attr = info.external_attr
                        ni.create_system = info.create_system
                        zout.writestr(ni, art[info.filename])
                        done.add(info.filename)
                    else:
                        zout.writestr(info, zin.read(info.filename), compress_type=info.compress_type)
                for path, blob in art.items():
                    if path not in done:
                        ni = zipfile.ZipInfo(path, (2020, 1, 1, 0, 0, 0))
                        ni.compress_type = zipfile.ZIP_DEFLATED
                        ni.external_attr = 0o644 << 16
                        zout.writestr(ni, blob)
            with zipfile.ZipFile(tmp) as chk:
                if chk.testzip() is not None:
                    raise ArtError('internal error: the new VPK failed its zip check')
            os.replace(tmp, dst)
        except BaseException:
            if os.path.exists(tmp):
                os.unlink(tmp)
            raise
    return sorted(art)


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description='Make the Galaxy Vita bubble and LiveArea pictures from your own game files.')
    ap.add_argument('game', help='folder with your own extracted Super Mario Galaxy files (LayoutData, ObjectData, ...)')
    ap.add_argument('--check', action='store_true', help='only check the folder and say what was found; writes nothing')
    ap.add_argument('--png-dir', help='write the pictures as loose files under this folder (sce_sys/...)')
    ap.add_argument('--vpk', help='the Galaxy-Vita.vpk to take as the base (never modified)')
    ap.add_argument('--out', help='with --vpk: where to write the new VPK')
    ap.add_argument('--pic0', action='store_true', help='also make the 960x544 pic0.png')
    a = ap.parse_args(argv)
    try:
        try:
            import PIL  # noqa: F401
        except ImportError:
            raise ArtError('Pillow is needed: pip install pillow')
        if a.vpk and not a.out and not a.check:
            raise ArtError('--vpk needs --out <new.vpk>')
        if a.vpk and not os.path.isfile(a.vpk):
            raise ArtError('VPK not found: %s' % a.vpk)
        if not a.check and not a.vpk and not a.png_dir:
            raise ArtError('nothing to do: give --check, --png-dir DIR, or --vpk IN --out OUT')
        info, logo = load_logo(a.game)
        print('Found your title logo: %s' % info['archive'])
        print('  texture %s, %dx%d, GX format %s, visible part %dx%d' % (
            info['texture'], info['size'][0], info['size'][1], info['format'], logo.width, logo.height))
        if a.check:
            if a.vpk:
                with zipfile.ZipFile(a.vpk) as z:
                    ok = all(n in z.namelist() for n in ('eboot.bin', 'sce_sys/livearea/contents/template.xml'))
                print('  VPK %s: %s' % (a.vpk, 'looks right' if ok else 'does not look like the Galaxy Vita VPK'))
            print('Check passed. Nothing was written.')
            return 0
        art = build_art(logo, a.pic0)
        if a.png_dir:
            for path, blob in art.items():
                dest = os.path.join(a.png_dir, *path.split('/'))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as f:
                    f.write(blob)
                print('  wrote %s' % dest)
        if a.vpk:
            patch_vpk(a.vpk, a.out, art)
            print('  wrote %s (replaced %s)' % (a.out, ', '.join(sorted(art))))
        print('Done. These pictures contain Nintendo art: keep them to yourself.')
        return 0
    except ArtError as ex:
        print('error: %s' % ex, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())

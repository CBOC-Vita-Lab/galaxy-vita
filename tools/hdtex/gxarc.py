"""Readers for the cooked (little-endian) game data: RARC archives, J3D models' TEX1
blocks and BTI images, as tools/cook leaves them (headers swapped to little-endian,
texel and palette bytes left big-endian: they are the bytes the game hands the GPU,
so a hash of them here equals a hash of guest memory at run time).
"""
import struct

FLAG_FOLDER = 0x02


def _u16(b, o): return struct.unpack_from('<H', b, o)[0]
def _u32(b, o): return struct.unpack_from('<I', b, o)[0]


def rarc_files(b):
    """[(path, bytes)] for every file of a cooked RARC ('CRAR')."""
    if b[:4] != b'CRAR':
        raise ValueError('not a cooked RARC')
    hdr = _u32(b, 8)
    data_start = hdr + _u32(b, 0xC)
    ndirs, dir_off, nfiles, file_off, _, str_off = (_u32(b, hdr + 4 * i) for i in range(6))
    dir_off += hdr; file_off += hdr; str_off += hdr

    def cstr(o):
        e = b.index(b'\0', str_off + o)
        return bytes(b[str_off + o:e]).decode('shift_jis', errors='replace')
    dirs = [(cstr(_u32(b, dir_off + 0x10 * i + 4)), _u16(b, dir_off + 0x10 * i + 0xA), _u32(b, dir_off + 0x10 * i + 0xC)) for i in range(ndirs)]
    out = []

    def walk(di, prefix, depth):
        if depth > 32:
            return
        name, count, first = dirs[di]
        for e in range(first, first + count):
            f = file_off + 0x14 * e
            word = _u32(b, f + 4)
            flags, name_off = word >> 24, word & 0xFFFFFF
            en = cstr(name_off)
            if flags & FLAG_FOLDER:
                if en in ('.', '..'):
                    continue
                walk(_u32(b, f + 8), prefix + en + '/', depth + 1)
            else:
                o = data_start + _u32(b, f + 8)
                out.append((prefix + en, b[o:o + _u32(b, f + 0xC)]))
    walk(0, dirs[0][0] + '/', 0)
    return out


# GX texture formats: (block w, block h, bits per texel)
FMT = {0x0: (8, 8, 4), 0x1: (8, 4, 8), 0x2: (8, 4, 8), 0x3: (4, 4, 16), 0x4: (4, 4, 16), 0x5: (4, 4, 16), 0x6: (4, 4, 32),
       0x8: (8, 8, 4), 0x9: (8, 4, 8), 0xA: (4, 4, 16), 0xE: (8, 8, 4)}
FMT_NAME = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3', 6: 'RGBA8', 8: 'C4', 9: 'C8', 0xA: 'C14X2', 0xE: 'CMPR'}


def level_size(w, h, fmt):
    bw, bh, bpp = FMT[fmt]
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    return bx * by * bw * bh * bpp // 8


class Timg:
    """One BTI header (0x20 bytes; cooked: multi-byte fields little-endian) and its level-0 data."""
    def __init__(self, buf, hoff, name=''):
        self.name = name
        self.fmt = buf[hoff]
        self.alpha = buf[hoff + 1]
        self.w, self.h = _u16(buf, hoff + 2), _u16(buf, hoff + 4)
        self.wrapS, self.wrapT = buf[hoff + 6], buf[hoff + 7]
        self.palFmt = buf[hoff + 9]
        self.palEntries = _u16(buf, hoff + 0xA)
        palOff = _u32(buf, hoff + 0xC)
        self.mipmap = buf[hoff + 0x10]
        self.minFilter, self.magFilter = buf[hoff + 0x14], buf[hoff + 0x15]
        self.mipCount = buf[hoff + 0x18]
        dataOff = _u32(buf, hoff + 0x1C)
        self.ok = self.fmt in FMT and 0 < self.w <= 1024 and 0 < self.h <= 1024
        n0 = level_size(self.w, self.h, self.fmt) if self.ok else 0
        self.level0 = bytes(buf[hoff + dataOff:hoff + dataOff + n0])
        if len(self.level0) != n0:
            self.ok = False
        self.palette = bytes(buf[hoff + palOff:hoff + palOff + self.palEntries * 2]) if self.fmt in (8, 9, 0xA) else b''


def model_textures(b):
    """TEX1 images of a cooked BMD/BDL (block tags are byte-swapped u32s)."""
    if len(b) < 0x20 or bytes(b[0:4]) not in (b'2D3J', b'1D3J'):
        return []
    n = _u32(b, 0x0C)
    o = 0x20
    out = []
    for _ in range(n):
        if o + 8 > len(b):
            break
        tag = bytes(b[o:o + 4])[::-1]
        size = _u32(b, o + 4)
        if tag == b'TEX1':
            num = _u16(b, o + 8)
            res, nameoff = _u32(b, o + 0xC), _u32(b, o + 0x10)
            names = name_table(b, o + nameoff) if nameoff else []
            for i in range(num):
                out.append(Timg(b, o + res + 0x20 * i, names[i] if i < len(names) else 'tex%d' % i))
        if size <= 0:
            break
        o += size
    return out


def bti_texture(b, name):
    return Timg(b, 0, name)


def name_table(b, o):
    n = _u16(b, o)
    names = []
    for i in range(n):
        so = _u16(b, o + 4 + 4 * i + 2)
        e = b.index(b'\0', o + so)
        names.append(bytes(b[o + so:e]).decode('shift_jis', errors='replace'))
    return names

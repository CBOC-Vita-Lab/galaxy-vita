"""Deterministic 8-bit palette PNG writing for the package pictures (make_livearea.py) and the host reference of the console's
picture path (make_livearea_title.py).

- quantize(): an exact Python port of the console's quantizer in platform/include/port/title_art_png.h (median cut on 6-bit cells,
  one refinement step, Floyd-Steinberg error diffusion in 1/16 units). Integer arithmetic only, so it gives the very same palette
  and indices as the C++ code, on any machine and with any NumPy.
- png8(): the PNG file (IHDR, PLTE, IDAT, IEND; 8-bit palette, no interlace, no transparency), compressed by the small deflate
  below (LZ77 over hash chains, dynamic Huffman blocks with package-merge code lengths). Not zlib: zlib and zlib-ng (which newer
  Pillow and Python builds use) give different bytes for the same input, and CI compares the shipped pictures byte for byte.
Needs NumPy only (for speed; every operation on pixel values is integer)."""
import struct
import zlib  # only for crc32 / adler32, which are exactly specified

import numpy as np

# ------------------------------------------------------------------------------------------------ quantizer (port of title_art_png.h)
WR, WG, WB = 3, 4, 2  # channel weights of the colour distance (title_art_png.h kWr, kWg, kWb)


def _keys(rgb):
    r, g, b = (rgb[..., i].astype(np.int64) for i in range(3))
    return ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)


def build_palette(rgb, max_colours=256):
    """BuildPalette: median cut over the 6-bit cells, splitting the box with the largest range^2 * pixel count."""
    keys = _keys(rgb).ravel()
    table = np.bincount(keys, minlength=262144)
    bin_keys = np.nonzero(table)[0].astype(np.int64)
    bin_counts = table[bin_keys].astype(np.int64)
    n = len(bin_keys)
    chans = np.stack([(bin_keys >> 12) & 63, (bin_keys >> 6) & 63, bin_keys & 63])  # 3 x n, permuted together with the bins
    boxes = [(0, n)]
    weights = (WR, WG, WB)

    def stats(b):
        lo = chans[:, b[0]:b[1]].min(axis=1)
        hi = chans[:, b[0]:b[1]].max(axis=1)
        axis, rng = 0, -1
        for j in range(3):
            if int(hi[j] - lo[j]) * weights[j] > rng:
                rng, axis = int(hi[j] - lo[j]) * weights[j], j
        return axis, rng, int(bin_counts[b[0]:b[1]].sum())

    cache = [stats(boxes[0])]
    max_colours = max(1, min(256, max_colours))
    while len(boxes) < max_colours:
        best, best_axis, best_score = -1, 0, 0
        for i, b in enumerate(boxes):
            if b[1] - b[0] < 2:
                continue
            axis, rng, total = cache[i]
            if rng <= 0:
                continue
            score = rng * rng * total
            if score > best_score:
                best, best_axis, best_score = i, axis, score
        if best < 0:
            break
        b0, b1 = boxes[best]
        order = np.lexsort((bin_keys[b0:b1], chans[best_axis, b0:b1])) + b0
        bin_keys[b0:b1] = bin_keys[order]
        bin_counts[b0:b1] = bin_counts[order]
        chans[:, b0:b1] = chans[:, order]
        total = int(bin_counts[b0:b1].sum())
        cs = np.cumsum(bin_counts[b0:b1 - 1])
        hit = np.nonzero(cs * 2 >= total)[0]
        split = b0 + (int(hit[0]) if len(hit) else (b1 - 1 - b0 - 1)) + 1
        boxes[best] = (b0, split)
        boxes.append((split, b1))
        cache[best] = stats(boxes[best])
        cache.append(stats(boxes[-1]))
    pal = np.zeros((len(boxes), 3), np.int64)
    for i, (b0, b1) in enumerate(boxes):
        c = bin_counts[b0:b1]
        total = int(c.sum())
        for j in range(3):
            s = int((c * ((chans[j, b0:b1] << 2) | 2)).sum())
            pal[i, j] = min(255, (s + total // 2) // total)
    return pal


def nearest_table(pal):
    """Nearest palette entry for every 6-bit cell centre (first of equals), as detail::Nearest."""
    k = np.arange(262144, dtype=np.int64)
    centre = np.stack([((k >> 12) & 63) * 4 + 2, ((k >> 6) & 63) * 4 + 2, (k & 63) * 4 + 2], axis=1)
    out = np.zeros(262144, np.int64)
    for s in range(0, 262144, 16384):
        c = centre[s:s + 16384, None, :] - pal[None, :, :]
        d = WR * c[..., 0] ** 2 + WG * c[..., 1] ** 2 + WB * c[..., 2] ** 2
        out[s:s + 16384] = np.argmin(d, axis=1)  # argmin returns the first minimum
    return out


def refine_palette(rgb, pal):
    near = nearest_table(pal)
    flat = rgb.reshape(-1, 3).astype(np.int64)
    idx = near[_keys(flat)]
    pal = pal.copy()
    count = np.bincount(idx, minlength=len(pal))
    for j in range(3):
        sums = np.zeros(len(pal), np.int64)
        np.add.at(sums, idx, flat[:, j])
        nz = count > 0
        pal[nz, j] = (sums[nz] + count[nz] // 2) // count[nz]
    return pal


def map_to_palette(rgb, pal, dither=True):
    """MapToPalette: Floyd-Steinberg in 1/16 units, rounding the carried error half away from zero."""
    h, w = rgb.shape[:2]
    near = nearest_table(pal).tolist()
    if not dither:
        return np.asarray(near, np.int64)[_keys(rgb)].astype(np.uint8)
    src = rgb.astype(np.int64).tolist()
    p = pal.tolist()
    idx = np.zeros((h, w), np.uint8)
    cur = [0] * ((w + 2) * 3)
    for y in range(h):
        nxt = [0] * ((w + 2) * 3)
        row = src[y]
        out = [0] * w
        for x in range(w):
            s = row[x]
            o = (x + 1) * 3
            v = []
            for c in range(3):
                e = cur[o + c]
                t = s[c] + ((e + 8) // 16 if e >= 0 else -((-e + 8) // 16))
                v.append(0 if t < 0 else (255 if t > 255 else t))
            i = near[((v[0] >> 2) << 12) | ((v[1] >> 2) << 6) | (v[2] >> 2)]
            out[x] = i
            q = p[i]
            for c in range(3):
                e = v[c] - q[c]
                cur[o + 3 + c] += e * 7
                nxt[x * 3 + c] += e * 3
                nxt[o + c] += e * 5
                nxt[o + 3 + c] += e
        idx[y] = out
        cur = nxt
    return idx


def quantize(rgb, dither=True):
    """(indices h x w uint8, palette n x 3), exactly as port::titleart::Quantize."""
    rgb = np.ascontiguousarray(rgb, dtype=np.uint8)
    pal = build_palette(rgb)
    pal = refine_palette(rgb, pal)
    return map_to_palette(rgb, pal, dither), pal


# ------------------------------------------------------------------------------------------------ deflate
_LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
_LEN_EXTRA = [0] * 8 + [1] * 4 + [2] * 4 + [3] * 4 + [4] * 4 + [5] * 4 + [0]
_DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
              12289, 16385, 24577]
_DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
_LEN_CODE = [0] * 259
for _c in range(29):
    for _l in range(_LEN_BASE[_c], (_LEN_BASE[_c + 1] if _c < 28 else 259)):
        _LEN_CODE[_l] = _c
_LEN_CODE[258] = 28


def _dist_code(d):
    lo, hi = 0, 29
    while lo < hi:  # last code whose base <= d
        mid = (lo + hi + 1) // 2
        if _DIST_BASE[mid] <= d:
            lo = mid
        else:
            hi = mid - 1
    return lo


def _lengths(freq, limit):
    """Length-limited Huffman code lengths (package-merge). Deterministic: ties keep symbol order."""
    used = [(f, s) for s, f in enumerate(freq) if f]
    out = [0] * len(freq)
    if not used:
        return out
    if len(used) == 1:
        out[used[0][1]] = 1
        return out
    leaves = sorted(used)
    items = [(f, (s,)) for f, s in leaves]
    cur = list(items)
    for _ in range(limit - 1):
        packs = [(cur[i][0] + cur[i + 1][0], cur[i][1] + cur[i + 1][1]) for i in range(0, len(cur) - 1, 2)]
        cur = sorted(items + packs, key=lambda t: t[0])
    for _, syms in cur[:2 * len(leaves) - 2]:
        for s in syms:
            out[s] += 1
    return out


def _codes(lengths):
    max_bits = max(lengths) if lengths else 0
    bl_count = [0] * (max_bits + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code, next_code = 0, [0] * (max_bits + 2)
    for bits in range(1, max_bits + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    out = [0] * len(lengths)
    for s, l in enumerate(lengths):
        if l:
            c = next_code[l]
            next_code[l] += 1
            r = 0
            for _ in range(l):  # deflate sends Huffman codes most significant bit first: store them reversed
                r = (r << 1) | (c & 1)
                c >>= 1
            out[s] = r
    return out


class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, count):
        self.acc |= value << self.n
        self.n += count
        while self.n >= 8:
            self.out.append(self.acc & 255)
            self.acc >>= 8
            self.n -= 8

    def flush(self):
        if self.n:
            self.out.append(self.acc & 255)
        self.acc = self.n = 0


def _tokens(data, chain=48, nice=96):
    """Greedy LZ77 with one step of lazy matching: (literal) ints, or (length, distance) tuples."""
    n = len(data)
    head = {}
    prev = [0] * n
    toks = []
    i = 0

    def longest(pos):
        if pos + 3 > n:
            return 0, 0
        key = data[pos:pos + 3]
        cand = head.get(key, -1)
        best_len, best_dist, steps = 0, 0, 0
        limit = min(258, n - pos)
        while cand >= 0 and pos - cand <= 32768 and steps < chain:
            if data[cand + best_len:cand + best_len + 1] == data[pos + best_len:pos + best_len + 1]:
                l = 3
                while l < limit and data[cand + l] == data[pos + l]:
                    l += 1
                if l > best_len:
                    best_len, best_dist = l, pos - cand
                    if l >= nice:
                        break
            cand = prev[cand]
            steps += 1
        return best_len, best_dist

    def insert(pos):
        if pos + 3 <= n:
            key = data[pos:pos + 3]
            prev[pos] = head.get(key, -1)
            head[key] = pos

    while i < n:
        l, d = longest(i)
        if l >= 3 and l < nice and i + 1 < n:
            insert(i)
            l2, d2 = longest(i + 1)
            if l2 > l:
                toks.append(data[i])
                i += 1
                l, d = l2, d2
                for k in range(l):
                    insert(i + k)
                toks.append((l, d))
                i += l
                continue
            for k in range(1, l):
                insert(i + k)
            toks.append((l, d))
            i += l
            continue
        if l >= 3:
            for k in range(l):
                insert(i + k)
            toks.append((l, d))
            i += l
        else:
            insert(i)
            toks.append(data[i])
            i += 1
    return toks


def _block(bits, toks, last):
    lit = [0] * 286
    dist = [0] * 30
    for t in toks:
        if isinstance(t, tuple):
            lit[257 + _LEN_CODE[t[0]]] += 1
            dist[_dist_code(t[1])] += 1
        else:
            lit[t] += 1
    lit[256] = 1
    if not any(dist):
        dist[0] = 1
    ll = _lengths(lit, 15)
    dl = _lengths(dist, 15)
    used = [i for i, x in enumerate(dl) if x]
    if len(used) == 1:  # one distance code: give it a partner so that the code is complete
        dl[1 if used[0] == 0 else 0] = 1
    hlit = 286
    while hlit > 257 and ll[hlit - 1] == 0:
        hlit -= 1
    hdist = 30
    while hdist > 1 and dl[hdist - 1] == 0:
        hdist -= 1
    seq = ll[:hlit] + dl[:hdist]
    rle = []  # (symbol, extra bits value, extra bit count)
    i = 0
    while i < len(seq):
        v = seq[i]
        run = 1
        while i + run < len(seq) and seq[i + run] == v:
            run += 1
        if v == 0 and run >= 3:
            r = min(run, 138)
            rle.append((17, r - 3, 3) if r <= 10 else (18, r - 11, 7))
            i += r
            continue
        if v != 0 and run >= 4:
            rle.append((v, 0, 0))
            r = min(run - 1, 6)
            rle.append((16, r - 3, 2))
            i += 1 + r
            continue
        rle.append((v, 0, 0))
        i += 1
    cl_freq = [0] * 19
    for s, _, _ in rle:
        cl_freq[s] += 1
    cl = _lengths(cl_freq, 7)
    order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
    hclen = 19
    while hclen > 4 and cl[order[hclen - 1]] == 0:
        hclen -= 1
    bits.put(1 if last else 0, 1)
    bits.put(2, 2)
    bits.put(hlit - 257, 5)
    bits.put(hdist - 1, 5)
    bits.put(hclen - 4, 4)
    for k in range(hclen):
        bits.put(cl[order[k]], 3)
    clc = _codes(cl)
    for s, e, n in rle:
        bits.put(clc[s], cl[s])
        if n:
            bits.put(e, n)
    lc, dc = _codes(ll), _codes(dl)
    for t in toks:
        if isinstance(t, tuple):
            l, d = t
            c = _LEN_CODE[l]
            bits.put(lc[257 + c], ll[257 + c])
            if _LEN_EXTRA[c]:
                bits.put(l - _LEN_BASE[c], _LEN_EXTRA[c])
            c = _dist_code(d)
            bits.put(dc[c], dl[c])
            if _DIST_EXTRA[c]:
                bits.put(d - _DIST_BASE[c], _DIST_EXTRA[c])
        else:
            bits.put(lc[t], ll[t])
    bits.put(lc[256], ll[256])


def zlib_stream(data):
    """A zlib stream (RFC 1950/1951) of `data`; the same bytes on every machine."""
    data = bytes(data)
    toks = _tokens(data)
    bits = _Bits()
    step = 16384
    if not toks:
        bits.put(1, 1)
        bits.put(1, 2)
        bits.put(0, 7)  # fixed block holding only the end code
    for s in range(0, len(toks), step):
        _block(bits, toks[s:s + step], s + step >= len(toks))
    bits.flush()
    return b'\x78\xda' + bytes(bits.out) + struct.pack('>I', zlib.adler32(data) & 0xffffffff)


# ------------------------------------------------------------------------------------------------ PNG
def _chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def png8(idx, pal, entries=256):
    """An 8-bit palette PNG: IHDR, PLTE (always `entries` entries, unused ones black, as the shipped files have), IDAT, IEND."""
    h, w = idx.shape
    pal = np.asarray(pal, np.int64)
    assert len(pal) <= entries and int(idx.max()) < len(pal)
    plte = bytearray(entries * 3)
    for i, c in enumerate(pal.tolist()):
        plte[i * 3:i * 3 + 3] = bytes(c)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += idx[y].astype(np.uint8).tobytes()
    return (b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0)) + _chunk(b'PLTE', bytes(plte)) +
            _chunk(b'IDAT', zlib_stream(raw)) + _chunk(b'IEND', b''))


def decode_png8(data):
    """(indices, palette) of an 8-bit palette PNG (any filter), for the checks; raises on anything else."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, pal, ihdr = 8, b'', None, None
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            pal = np.frombuffer(body, np.uint8).reshape(-1, 3)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, inter = ihdr
    assert (depth, ctype, inter) == (8, 3, 0)
    raw = zlib.decompress(idat)
    out = np.zeros((h, w), np.uint8)
    prior = np.zeros(w, np.int64)
    for y in range(h):
        f = raw[y * (w + 1)]
        line = np.frombuffer(raw, np.uint8, w, y * (w + 1) + 1).astype(np.int64)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prior) & 255
        else:
            cur = np.zeros(w, np.int64)
            for x in range(w):
                a = cur[x - 1] if x else 0
                b = prior[x]
                c = prior[x - 1] if x else 0
                if f == 1:
                    cur[x] = (line[x] + a) & 255
                elif f == 3:
                    cur[x] = (line[x] + (a + b) // 2) & 255
                else:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    cur[x] = (line[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        out[y] = cur
        prior = cur
    return out, pal

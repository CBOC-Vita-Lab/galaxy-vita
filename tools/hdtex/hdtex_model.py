"""What a finished pack costs and how much of it the picture can use: reading a pack, DXT1/DXT5 decoding (numpy), the mip chain, the quality
numbers, per-scene GPU estimates and the screen-size model. No game data and no third-party packages beyond numpy are needed to read a pack:
`build_pack.py --report <pack>` runs everything here on a pack alone.

Screen-size model (estimates, tools/hdtex/screensize.txt; to be checked on the console with paused pictures):
  A texture of W x H original texels is spread over an object whose long side is E pixels on the 960x544 screen; the visible part of the texture's
  long side is `UV_SPAN` (0.7) of it. The original has uL = 0.7 * max(W, H) texels across the object; the 2x replacement has twice that. The GPU samples
  the replacement's level 0 only where it has no more than one texel per pixel (LOD <= 0), i.e. E >= 2 * uL; between E = uL and 2 * uL level 0 blends
  with level 1 (the original's size), and below E = uL the picture is made of level 1 and smaller, which are the replacement's own downscaled
  images: sharper edges and no block noise, but no more detail than the original had. So `detail_px` = uL is the object size above which the pack
  shows anything more than the game does.
"""
import os
import re
import struct

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PACK_DXT1, PACK_DXT5 = 1, 3
UV_SPAN = 0.7
GX_FMT = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3', 6: 'RGBA8', 8: 'C4', 9: 'C8', 10: 'C14X2', 14: 'CMPR'}
PRELOAD_BYTES = 8 << 20  # GXV_HDTEX_PRELOAD's default (vgx_hdtex.cpp)


# ------------------------------------------------------------------------------------------------ the pack
def level_sizes(w, h, levels, fmt):
    """[(w, h, bytes)] of the stored levels (level 0 first)."""
    bs = 16 if fmt == PACK_DXT5 else 8
    out = []
    for _ in range(levels):
        out.append((w, h, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs))
        w, h = max(1, w // 2), max(1, h // 2)
    return out


def console_levels(w, h, levels):
    """How many levels the console takes: down to the last of at least 4x4 (vgx_hdtex.cpp, Caps::mipClamp)."""
    n = 1
    while n < levels and (w >> 1) >= 4 and (h >> 1) >= 4:
        w >>= 1
        h >>= 1
        n += 1
    return n


def read_pack(path):
    """Header and index of a pack, checked the way vgx_hdtex.cpp checks them. Returns (header dict, [entry dict], problems [str]).
    An entry: key, off, size, w, h, ow, oh, fmt, levels, ofmt, alpha, name, names (list of "Archive:texture"), console_bytes, all_bytes, orig_bytes."""
    with open(path, 'rb') as f:
        b = f.read()
    problems = []
    if len(b) < 32:
        return {}, [], ['file shorter than the 32-byte header']
    magic, ver, n, io, no, do, size, rsv = struct.unpack_from('<4sIIIIIII', b, 0)
    hdr = dict(magic=magic, version=ver, count=n, index_off=io, names_off=no, data_off=do, size=size, file_size=len(b))
    if magic != b'HDTX' or ver != 1:
        problems.append('not a version 1 HDTX pack')
        return hdr, [], problems
    if size > len(b):
        problems.append('header says %d bytes, the file has %d (copied in part?)' % (size, len(b)))
    if n >= 65536 or io < 32 or no < io + 32 * n or do < no:
        problems.append('header offsets inconsistent')
        return hdr, [], problems
    entries, last = [], -1
    for i in range(n):
        k, off, sz, w, h, ow, oh, fmt, lv, ofmt, fl, nmo = struct.unpack_from('<QIIHHHHBBBBI', b, io + 32 * i)
        if k <= last:
            problems.append('index not sorted at record %d' % i)
        last = k
        if fmt not in (PACK_DXT1, PACK_DXT5) or lv < 1 or not w or not h or (w & (w - 1)) or (h & (h - 1)):
            problems.append('record %016x: unusable format/size' % k)
        ls = level_sizes(w, h, lv, fmt)
        if sum(x[2] for x in ls) != sz:
            problems.append('record %016x: size %d is not the sum of its levels %d' % (k, sz, sum(x[2] for x in ls)))
        if off + sz > len(b):
            problems.append('record %016x: data past the end of the file' % k)
        if off % 64:
            problems.append('record %016x: data not 64-byte aligned' % k)
        e = b.index(b'\0', no + nmo) if no + nmo < len(b) else no + nmo
        name = b[no + nmo:e].decode('ascii', 'replace')
        cl = console_levels(w, h, lv)
        ob = (ow * oh * 4) if (fl & 1) else max(1, ow // 4) * max(1, oh // 4) * 8  # what the game's own image holds (alpha: decoded to RGBA8; else DXT1), level 0
        entries.append(dict(key=k, off=off, size=sz, w=w, h=h, ow=ow, oh=oh, fmt=fmt, levels=lv, ofmt=ofmt, alpha=bool(fl & 1), name=name, names=name.split(' '),
                            console_bytes=sum(x[2] for x in ls[:cl]), all_bytes=sz, orig_bytes=ob, console_levels=cl, data=b[off:off + sz]))
    return hdr, entries, problems


# ------------------------------------------------------------------------------------------------ DXT decoding
def _rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _bc1_blocks(blk):
    """blk: (N, 8) uint8 -> (N, 16, 4) RGBA int32 (texels row-major in the block)."""
    c0 = blk[:, 0].astype(np.uint32) | (blk[:, 1].astype(np.uint32) << 8)
    c1 = blk[:, 2].astype(np.uint32) | (blk[:, 3].astype(np.uint32) << 8)
    idx = blk[:, 4].astype(np.uint32) | (blk[:, 5].astype(np.uint32) << 8) | (blk[:, 6].astype(np.uint32) << 16) | (blk[:, 7].astype(np.uint32) << 24)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # (N, 4, 3)
    a3 = np.where(four[:, 0], 255, 0)
    sel = np.stack([(idx >> (2 * t)) & 3 for t in range(16)], 1)  # (N, 16)
    rgb = np.take_along_axis(pal, sel[..., None].repeat(3, -1), 1)
    alpha = np.where(sel == 3, a3[:, None], 255)
    return np.concatenate([rgb, alpha[..., None]], -1)


def _bc3_alpha(blk):
    a0, a1 = blk[:, 0].astype(np.int32), blk[:, 1].astype(np.int32)
    bits = np.zeros(len(blk), np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros((len(blk), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    seven = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(seven, ((7 - i) * a0 + i * a1) // 7, np.where(i <= 4, ((5 - i) * a0 + i * a1) // 5, 0))
    pal[:, 7] = np.where(seven, pal[:, 7], 255)
    sel = np.stack([((bits >> np.uint64(3 * t)) & np.uint64(7)).astype(np.int64) for t in range(16)], 1)
    return np.take_along_axis(pal, sel, 1)


def decode_dxt(data, w, h, fmt):
    """DXT1 (fmt 1) or DXT5 (fmt 3) level -> (h, w, 4) uint8. Rounding of the interpolated colours can differ from a GPU's by one level."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bs = 16 if fmt == PACK_DXT5 else 8
    raw = np.frombuffer(data[:bw * bh * bs], np.uint8).reshape(bw * bh, bs)
    if fmt == PACK_DXT5:
        px = _bc1_blocks(raw[:, 8:])
        px[..., 3] = _bc3_alpha(raw[:, :8])
    else:
        px = _bc1_blocks(raw)
    px = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.clip(px[:h, :w], 0, 255).astype(np.uint8)


# ------------------------------------------------------------------------------------------------ mip chain
def make_mips(hd, gamma=1.0, alpha_weighted=True):
    """Mip chain of a float RGBA image (values 0..1, shape (H, W, 4), power-of-two sides) down to 1x1: each level is the area average of
    the one above (exactly a box filter of level 0). Colours are averaged weighted by alpha, when the texture has any (a texel that is
    half transparent counts half: no dark or light fringe from the colour of transparent texels; with the builder's filled-in colours
    this changes little, with raw ones it matters), and in the encoded values (gamma = 1: what the GX hardware and the game's own mip
    levels do; the look of a minified texture then stays the original's) or, gamma > 1, in linear light (value ** gamma) for a physically
    correct blend (brighter minified high-contrast patterns than the game ever showed). Alpha is the plain average (coverage)."""
    lv = [hd.astype(np.float32)]
    while lv[-1].shape[0] > 1 or lv[-1].shape[1] > 1:
        a = lv[-1]
        h, w = a.shape[:2]
        nh, nw = max(1, h // 2), max(1, w // 2)
        sy, sx = (2 if h > 1 else 1), (2 if w > 1 else 1)
        col = np.clip(a[..., :3], 0, 1)
        if gamma != 1.0:
            col = col ** gamma
        al = a[..., 3:4]
        wgt = al if alpha_weighted else np.ones_like(al)
        num = (col * wgt).reshape(nh, sy, nw, sx, 3).sum((1, 3))
        den = wgt.reshape(nh, sy, nw, sx, 1).sum((1, 3))
        plain = col.reshape(nh, sy, nw, sx, 3).mean((1, 3))
        c = np.where(den > 1e-6, num / np.maximum(den, 1e-6), plain)
        if gamma != 1.0:
            c = c ** (1.0 / gamma)
        aa = al.reshape(nh, sy, nw, sx, 1).mean((1, 3))
        lv.append(np.concatenate([c, aa], -1).astype(np.float32))
    return lv


# ------------------------------------------------------------------------------------------------ quality numbers
def psnr_rgb(ref, dec, visible=None):
    """PSNR (dB) of the colour channels of two RGBA uint8 images over the texels the picture shows (alpha > 0 in the reference, or `visible`).
    Alpha is not in it: an opaque texture's constant alpha would raise the figure by 1.25 dB."""
    if visible is None:
        visible = ref[..., 3] > 0
    if not visible.any():
        return 99.0
    d = (ref[..., :3].astype(np.float32) - dec[..., :3].astype(np.float32))[visible]
    mse = float(np.mean(d * d))
    return 99.0 if mse == 0 else min(99.0, 10 * np.log10(255.0 ** 2 / mse))


def worst_block_psnr(ref, dec):
    """The lowest PSNR (dB) of any 4x4 block's colour channels: where the compression is worst (banding, ringing at an edge)."""
    h, w = ref.shape[:2]
    h4, w4 = h // 4 * 4, w // 4 * 4
    if h4 == 0 or w4 == 0:
        return 99.0
    d = (ref[:h4, :w4, :3].astype(np.float32) - dec[:h4, :w4, :3].astype(np.float32)) ** 2
    mse = d.reshape(h4 // 4, 4, w4 // 4, 4, 3).mean((1, 3, 4))
    worst = float(mse.max())
    return 99.0 if worst == 0 else min(99.0, 10 * np.log10(255.0 ** 2 / worst))


def mip_consistency_db(dec0, dec1):
    """From a pack alone: how far the stored level 1 is from the area average of the stored level 0 (each is compressed on its own, so
    this is about the two compression errors together, not about the upscaling). Colour channels, texels with alpha in level 0."""
    h, w = dec1.shape[:2]
    a = dec0[:2 * h, :2 * w].astype(np.float32)
    box = a.reshape(h, 2, w, 2, 4).mean((1, 3))
    d = (box[..., :3] - dec1[..., :3].astype(np.float32))
    vis = box[..., 3] > 0
    if not vis.any():
        return 99.0
    mse = float(np.mean(d[vis] ** 2))
    return 99.0 if mse == 0 else min(99.0, 10 * np.log10(255.0 ** 2 / mse))


def pack_quality(e):
    """Numbers computed from the pack's own images: mip consistency (dB), the share of alpha values that are neither 0 nor 255, the mean colour
    of the fully transparent texels (a black one shows as dark fringes), and the seam ratio (mean step across the wrap-around seam
    over the mean step between neighbouring rows / columns: about 1 for a texture that tiles)."""
    d0 = decode_dxt(e['data'], e['w'], e['h'], e['fmt'])
    out = {}
    if e['levels'] > 1 and e['w'] >= 2 and e['h'] >= 2:
        sizes = level_sizes(e['w'], e['h'], 2, e['fmt'])
        d1 = decode_dxt(e['data'][sizes[0][2]:sizes[0][2] + sizes[1][2]], sizes[1][0], sizes[1][1], e['fmt'])
        out['mip_db'] = mip_consistency_db(d0, d1)
    a = d0[..., 3]
    out['partial_alpha'] = float(np.mean((a > 0) & (a < 255))) if e['alpha'] else 0.0
    clear = a == 0
    out['clear_texels'] = float(clear.mean())
    out['clear_rgb'] = float(d0[..., :3][clear].mean()) if clear.any() else -1.0
    f = d0[..., :3].astype(np.float32)
    vy = np.abs(np.diff(f, axis=0)).mean() if f.shape[0] > 1 else 1.0
    vx = np.abs(np.diff(f, axis=1)).mean() if f.shape[1] > 1 else 1.0
    sy = np.abs(f[0] - f[-1]).mean()
    sx = np.abs(f[:, 0] - f[:, -1]).mean()
    out['seam_ratio'] = float(max(sy / max(vy, 0.5), sx / max(vx, 0.5)))
    return out


# ------------------------------------------------------------------------------------------------ screen size and scenes
def read_screensize(path=None):
    """[(compiled regex, typical_px, closeup_px)] from screensize.txt: first matching line wins."""
    path = path or os.path.join(HERE, 'screensize.txt')
    out = []
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            line = line.split('#', 1)[0].split()
            if len(line) >= 3:
                out.append((re.compile(line[0]), int(line[1]), int(line[2])))
    return out


def screen_note(names, ow, oh, table, span=UV_SPAN):
    """(detail_px, typical_px, closeup_px, verdict) for a texture: see the module comment. typical / closeup are None without a table line."""
    detail = span * max(ow, oh)
    typ = clo = None
    for tok in names:
        for rx, t, c in table:
            if rx.search(tok):
                typ, clo = t, c
                break
        if typ is not None:
            break
    if typ is None:
        return detail, None, None, 'unknown'
    if typ >= 2 * detail:
        v = 'gameplay'          # level 0 resolved at the usual distance
    elif typ >= detail:
        v = 'gameplay-partly'   # level 0 blended in at the usual distance
    elif clo >= detail:
        v = 'closeups'          # only cutscenes and close-ups reach the extra detail
    else:
        v = 'never'
    return detail, typ, clo, v


# Scenes the estimates are made for: [(label, [regexes over "Archive:texture" names])]. A record is alive in a scene when one of its names matches.
# (Which textures a real scene uploads depends on what it draws: these are the textures of the characters and items that stand in it.)
_MARIO = r'^(Mario:(?!MarioBody\.[12]$|MarioCap\.[12]$)|MarioHair:)'
SCENES = [
    ('typical stage: Mario', [_MARIO]),
    ('stage with the Bee Mushroom: Mario, Bee Mario, the item', [_MARIO, r'^BeeMario:', r'^PowerUpBee:']),
    ('Boo Mario stage', [_MARIO, r'^TeresaMario:', r'^PowerUpTeresa:']),
    ('two players: Mario and Luigi', [_MARIO, r'^Luigi:(?!MarioBody\.[12]$|MarioCap\.[12]$)']),
    ('Comet Observatory: Mario, Rosalina, Lumas, Toads', [_MARIO, r'^Rosetta:', r'^(Tico|TicoFat|TicoShop|SpinTico|TicoComet|TicoBaby):', r'^(Kinopio|Kinopi):']),
    ('Bowser fight: Mario, Bowser', [_MARIO, r'^Koopa:']),
    ('Bowser Jr. fight: Mario, Bowser Jr.', [_MARIO, r'^KoopaJr:']),
    ('story scene with Peach: Mario, Peach', [_MARIO, r'^(Peach|PeachHair):']),
    ('every record of the pack', [r'.']),
]


def scene_bytes(entries, regexes):
    rx = [re.compile(r) for r in regexes]
    sel = [e for e in entries if any(r.search(t) for t in e['names'] for r in rx)]
    return sel, sum(e['console_bytes'] for e in sel), sum(e['orig_bytes'] for e in sel)


# ------------------------------------------------------------------------------------------------ the report
def report(path, manifest=None, floor=None, out=print, quality=True, screen=None):
    """Prints what a pack costs and what to look at; returns a dict of the headline numbers (also for the tests)."""
    hdr, entries, problems = read_pack(path)
    res = dict(problems=problems, records=len(entries))
    out('pack %s: %d bytes, %d records, version %s' % (path, hdr.get('file_size', 0), len(entries), hdr.get('version')))
    for p in problems:
        out('  PROBLEM: ' + p)
    if not entries:
        return res
    table = read_screensize(screen)
    tot_all = sum(e['all_bytes'] for e in entries)
    tot_con = sum(e['console_bytes'] for e in entries)
    tot_orig = sum(e['orig_bytes'] for e in entries)
    res.update(file_bytes=hdr['file_size'], all_bytes=tot_all, console_bytes=tot_con, orig_bytes=tot_orig)
    nd5 = sum(1 for e in entries if e['fmt'] == PACK_DXT5)
    out('  formats: %d DXT1, %d DXT5; shapes (original format/size): %d' % (len(entries) - nd5, nd5, len({(e['ofmt'], e['ow'], e['oh']) for e in entries})))
    out('  GPU memory if every record were alive: %.2f MiB on the console (levels down to 4x4; %.2f MiB all levels); the game\'s own images of the same textures: about %.2f MiB'
        % (tot_con / 1048576.0, tot_all / 1048576.0, tot_orig / 1048576.0))
    out('  main memory: %.2f MiB when preloaded (GXV_HDTEX_PRELOAD default 8 MiB: %s)' % (hdr['file_size'] / 1048576.0,
        'whole pack in memory' if hdr['file_size'] <= PRELOAD_BYTES else 'only the first 8 MiB in memory: later records are read from the card on use'))
    lvl0 = sum(level_sizes(e['w'], e['h'], 1, e['fmt'])[0][2] for e in entries)
    out('  of which level 0 (the 2x image): %.2f MiB = %.0f %% of the bytes' % (lvl0 / 1048576.0, 100.0 * lvl0 / max(1, tot_all)))
    out('  scenes (records alive together; HD bytes / the game\'s own images of those textures / extra):')
    res['scenes'] = {}
    for label, rx in SCENES:
        sel, hd, orig = scene_bytes(entries, rx)
        res['scenes'][label] = (len(sel), hd, orig)
        out('    %-58s %3d records %7.0f KiB / %6.0f KiB / +%6.0f KiB' % (label, len(sel), hd / 1024.0, orig / 1024.0, (hd - orig) / 1024.0))
    by = {}
    for e in entries:
        d, t, c, v = screen_note(e['names'], e['ow'], e['oh'], table)
        e['verdict'], e['detail_px'] = v, d
        by.setdefault(v, [0, 0])
        by[v][0] += 1
        by[v][1] += e['console_bytes']
    res['verdicts'] = {k: tuple(v) for k, v in by.items()}
    if table:
        out('  screen-size verdicts (960x544; estimates from screensize.txt: does the 2x level 0 ever get sampled?):')
        names = {'gameplay': 'level 0 resolved at the usual distance', 'gameplay-partly': 'level 0 blended in at the usual distance',
                 'closeups': 'extra detail only in close-ups and cutscenes', 'never': 'not even in close-ups', 'unknown': 'no screensize line'}
        for k in ('gameplay', 'gameplay-partly', 'closeups', 'never', 'unknown'):
            if k in by:
                out('    %-16s %3d records %7.0f KiB  %s' % (k, by[k][0], by[k][1] / 1024.0, names[k]))
    big = sorted(entries, key=lambda e: -e['console_bytes'])[:8]
    out('  largest: ' + '; '.join('%s %dx%d %d KiB' % (e['names'][0], e['w'], e['h'], e['console_bytes'] // 1024) for e in big))
    if quality:
        qs = [(e, pack_quality(e)) for e in entries]
        mip = [(q['mip_db'], e['names'][0]) for e, q in qs if 'mip_db' in q]
        if mip:
            res['mip_min_db'] = min(mip)[0]
            out('  mip consistency (level 1 against the average of level 0, from the pack alone): worst %.1f dB (%s), median %.1f dB' %
                (min(mip)[0], min(mip)[1], float(np.median([m for m, _ in mip]))))
        partial = [(q['partial_alpha'], e['names'][0]) for e, q in qs if e['alpha']]
        if partial:
            out('  alpha textures: %d; most partial (neither 0 nor 255) alpha at level 0: %.1f %% (%s)' % (len(partial), 100 * max(partial)[0], max(partial)[1]))
        dark = [(q['clear_rgb'], e['names'][0]) for e, q in qs if q['clear_rgb'] >= 0]
        if dark:
            out('  colour under fully transparent texels: darkest mean %.0f / 255 (%s) (near 0: dark fringes where they are filtered)' % (min(dark)[0], min(dark)[1]))
        seam = sorted([(q['seam_ratio'], e['names'][0]) for e, q in qs], reverse=True)[:3]
        out('  largest wrap-around seam steps (ratio to the step between neighbouring rows/columns; ~1 tiles; flag only if the texture repeats): ' +
            ', '.join('%s %.1f' % (n, r) for r, n in seam))
    if manifest and os.path.exists(manifest):
        with open(manifest) as f:
            rows = [l.rstrip('\n').split('\t') for l in f]
        head, rows = rows[0], rows[1:]
        if 'psnr_db' in head:
            i = head.index('psnr_db')
            vals = sorted((float(r[i]), r[1].split(' ')[0]) for r in rows)
            res['worst_psnr'] = vals[0][0]
            out('  compression PSNR (colour, level 0, build time): worst %.1f dB (%s), median %.1f dB; the lowest: %s' %
                (vals[0][0], vals[0][1], float(np.median([v for v, _ in vals])), ', '.join('%s %.1f' % (n, v) for v, n in vals[:5])))
            if floor is not None:
                low = [n for v, n in vals if v < floor]
                out('  below the %.0f dB floor: %s' % (floor, ', '.join(low) if low else 'none'))
    warn = []
    if hdr['file_size'] > PRELOAD_BYTES:
        warn.append('the pack is larger than the 8 MiB preload: records past it are read from the card when first used in play')
    if tot_con > 16 << 20:
        warn.append('more than 16 MiB of GPU memory if everything is alive (GXV_HDTEX_BUDGET default 16 MiB: later textures stay original)')
    for w in warn:
        out('  WARNING: ' + w)
    res['warnings'] = warn
    return res

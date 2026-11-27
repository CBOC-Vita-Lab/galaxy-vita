#!/usr/bin/env python3
"""Builds the HD texture pack (hdtex.pack) for the vgx renderer from the cooked game data.

    python3 -E -P tools/hdtex/build_pack.py [--data cooked/files] [--out hdtex-build]
        [--model hdtex-build/models/realesr-general-x4v3.pth] [--scale 2]

Reads the archives named in selection.txt in place, never copies game data anywhere else.
Writes <out>/hdtex.pack, <out>/manifest.tsv and <out>/sheets/*.png (before/after contact
sheets), and caches the network's output in <out>/cache/ (a rerun only re-encodes).
Nothing it writes belongs in git. Pack format: tools/hdtex/README.md.

Per texture: decode the GX level 0 -> fill the colour of fully transparent texels from their
neighbours (so neither the network nor the filtering drags black into the edges) -> Real-ESRGAN
4x (realesr-general-x4v3; alpha upscaled the same way, as grey) -> Lanczos down to `scale`x ->
mip chain by area averaging down to 1x1 (hdtex_model.make_mips) -> BC1 (opaque) or BC3 (any alpha below 255),
kept only above a compression-quality floor (--psnr-floor: below it the network's detail is blended back toward a plain
Lanczos enlargement of the original, 100 / 70 / 40 %, and a texture that still misses the floor stays out of the pack:
the game's own image is used).
    python3 -E -P tools/hdtex/build_pack.py --report <pack> [--manifest manifest.tsv]
prints what a finished pack costs (GPU and main memory, per scene) and how much of it the picture can use; needs no game data.
Needs: numpy, Pillow, xxhash, torch (CPU), quicktex (--report: numpy only).
"""
import argparse
import os
import re
import struct
import sys
import time

import numpy as np
import xxhash
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxarc  # noqa: E402
import gxdecode  # noqa: E402
import hdtex_model  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
COLOUR_FORMATS = {0x4, 0x5, 0x6, 0x8, 0x9, 0xA, 0xE}
EXCLUDE_NAMES = re.compile(r'(?i)(env|high_?light|hilight|spec|noise|noizu|^ind|ind$|testind|dummy|mask|fooline)')
MIN_DIM = 16
PACK_DXT1, PACK_DXT5 = 1, 3
# Compression-quality floor: PSNR (dB, colour channels, texels the picture shows, level 0 against the image that went into the encoder). On synthetic
# 256x256 textures through this encoder (tests/test_hdtex_builder.py): smooth gradients 43-51 dB, hard-edged art 51, soft cloth noise 40, fine noise 29,
# white noise 19. 34 dB separates what BC1/BC3 carries from what it turns to block noise.
PSNR_FLOOR = 34.0
BLEND_LADDER = (1.0, 0.7, 0.4)  # the network's share of a texture that misses the floor, before it is left out


def tex_key(fmt, w, h, level0, palette):
    """Must match HdKey() in platform/vgx/src/vgx_hdtex.cpp."""
    seed = fmt | (w << 8) | (h << 20)
    k = xxhash.xxh3_64_intdigest(level0, seed=seed)
    if palette:
        k = xxhash.xxh3_64_intdigest(palette, seed=k)
    return k


def pack_bytes(w, h, alpha):
    """Bytes of an HD pack entry (the whole BC1/BC3 mip chain down to 1x1) for w x h level 0: what the GPU holds for it."""
    blk, tot = (16 if alpha else 8), 0
    while True:
        tot += ((w + 3) // 4) * ((h + 3) // 4) * blk
        if w == 1 and h == 1:
            return tot
        w, h = max(1, w // 2), max(1, h // 2)


def read_selection(path):
    out = []
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if line:
            parts = line.split(None, 1)
            out.append((parts[0], re.compile(parts[1]) if len(parts) > 1 else None))
    return out


# The cooked (flat layout) game data: tools/cook/cook.py output, or HDTEX_DATA.
DEFAULT_DATA = os.environ.get('HDTEX_DATA', os.path.join('cooked', 'files'))


def all_archives(data_root):
    """Every archive of ObjectData/ and StageData/ (names as selection.txt gives them; StageData ones with their folder)."""
    out = []
    for sub in ('ObjectData', 'StageData'):
        d = os.path.join(data_root, sub)
        if os.path.isdir(d):
            out += [((sub + '/' if sub != 'ObjectData' else '') + f[:-4], None) for f in sorted(os.listdir(d)) if f.endswith('.arc')]
    return out


def read_keylist(path):
    """A census selection (tools/hdtex/select_from_census.py): '<16 hex digits>' or '<archive>:<texture>' per line, '#' comments.
    Returns (set of keys, set of 'archive:texture')."""
    keys, names = set(), set()
    for line in open(path):
        line = line.split('#', 1)[0].strip().split('\t')[0].strip()
        if not line:
            continue
        if re.fullmatch(r'[0-9a-fA-F]{16}', line):
            keys.add(int(line, 16))
        else:
            names.add(line)
    return keys, names


def collect(data_root, selection, only=None):
    """{key: dict} of the selected textures, deduplicated by content. only=(keys, names) (read_keylist): keep just those."""
    texs = {}
    for si, (arc, rx) in enumerate(selection):
        # (a name without a directory is an ObjectData archive; "LayoutData/Name" or any folder of the converted copy's files/ works too)
        path = os.path.join(data_root, *(arc.split('/') if '/' in arc else ['ObjectData', arc])) + '.arc'
        if not os.path.exists(path):
            print('missing archive', arc)
            continue
        for fpath, d in gxarc.rarc_files(open(path, 'rb').read()):
            if fpath.endswith(('.bdl', '.bmd')):
                ts = gxarc.model_textures(d)
            elif fpath.endswith('.bti'):
                ts = [gxarc.bti_texture(d, os.path.basename(fpath))]
            else:
                continue
            for t in ts:
                why = None
                if not t.ok:
                    why = 'unreadable'
                elif t.fmt not in COLOUR_FORMATS:
                    why = 'intensity format'
                elif EXCLUDE_NAMES.search(t.name):
                    why = 'name rule'
                elif t.w < MIN_DIM or t.h < MIN_DIM:
                    why = 'small'
                elif (t.w & (t.w - 1)) or (t.h & (t.h - 1)):
                    why = 'not a power of two'
                elif rx and not rx.search(t.name):
                    why = 'selection regex'
                if why:
                    continue
                k = tex_key(t.fmt, t.w, t.h, t.level0, t.palette)
                if only is not None and k not in only[0] and '%s:%s' % (arc, t.name) not in only[1]:
                    continue
                if k in texs:
                    if '%s:%s' % (arc, t.name) not in texs[k]['names']:
                        texs[k]['names'].append('%s:%s' % (arc, t.name))
                    continue
                texs[k] = dict(key=k, names=['%s:%s' % (arc, t.name)], t=t, prio=si)
    return texs


def bleed(rgb, valid, iters=64):
    """Gives fully transparent texels the average colour of their valid neighbours, growing outwards."""
    rgb = rgb.copy()
    valid = valid.copy()
    if valid.all() or not valid.any():
        return rgb
    h, w = valid.shape
    for _ in range(iters):
        if valid.all():
            break
        pr = np.pad(rgb * valid[..., None], ((1, 1), (1, 1), (0, 0)), mode='edge')
        pv = np.pad(valid.astype(np.float32), ((1, 1), (1, 1)), mode='edge')
        s = np.zeros_like(rgb)
        c = np.zeros(valid.shape, np.float32)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                if dy == 1 and dx == 1:
                    continue
                s += pr[dy:dy + h, dx:dx + w]
                c += pv[dy:dy + h, dx:dx + w]
        grow = (~valid) & (c > 0)
        rgb[grow] = s[grow] / c[grow][:, None]
        valid |= grow
    return rgb


def resize_f(a, w, h, flt):
    """Float (H, W, C) array -> (h, w, C) with PIL's filter, channel by channel."""
    return np.stack([np.asarray(Image.fromarray(a[..., c].astype(np.float32), 'F').resize((w, h), flt)) for c in range(a.shape[2])], -1)


def encode(rgba8, w, h, fmt):
    import quicktex
    import quicktex.s3tc.bc1 as bc1
    import quicktex.s3tc.bc3 as bc3
    pw, ph = max(4, (w + 3) // 4 * 4), max(4, (h + 3) // 4 * 4)
    if (pw, ph) != (w, h):
        rgba8 = np.pad(rgba8, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    raw = quicktex.RawTexture.frombytes(np.ascontiguousarray(rgba8).tobytes(), pw, ph)
    if fmt == PACK_DXT1:
        tex = bc1.BC1Encoder(18).encode(raw)
        dec = bc1.BC1Decoder().decode(tex)
    else:
        tex = bc3.BC3Encoder(18).encode(raw)
        dec = bc3.BC3Decoder().decode(tex)
    out = np.frombuffer(dec.tobytes(), np.uint8).reshape(ph, pw, 4)[:h, :w]
    return tex.tobytes(), out


def pad_mode(src, axis, wrap):
    """How to pad the borders of one axis for the network (numpy pad mode).
    A repeating texture is padded with its other side only when it really tiles across that
    seam: many textures are flagged repeat but are atlases or gradients whose opposite borders
    differ. Padded with wrap-around, such a texture shows the network a hard step at the seam,
    which it sharpens like any edge: darker on one side, brighter on the other (the Power Star's
    dark right and bottom band). Otherwise, and for mirrored textures, mirror padding (no step);
    clamped textures repeat their border texels, as the sampler does."""
    if wrap == 0:
        return 'edge'
    if wrap == 1:
        a = np.moveaxis(src, axis, 0)
        seam = np.abs(a[0] - a[-1]).mean()
        inner = np.abs(np.diff(a, axis=0)).mean() if a.shape[0] > 1 else 0.0
        if seam <= 2 * inner + 2 / 255:
            return 'wrap'
    return 'symmetric'


FIDELITY_T = 4 / 255  # deviations from the original (area-averaged) up to this are the network's to keep


def fidelity(hd, src, shown, opaque, iters=3):
    """Soft-thresholded back-projection: where the replacement, area-averaged to the original's
    size, differs from the original by more than FIDELITY_T, the excess is spread back over it
    (bilinearly), a few times. Corrects what the network gets wrong at the original's scale (a
    thin line along a border washed out, a uniform brightening of near-white areas, an alpha rim)
    while keeping what it adds below one texel and the CMPR block noise it removes (smaller than
    the threshold). `src`: the original as float RGBA (transparent texels' colour filled in)."""
    h, w = src.shape[:2]
    H, W = hd.shape[:2]
    for _ in range(iters):
        r = src - resize_f(hd, w, h, Image.BOX)
        r[..., :3] *= shown[..., None]
        if opaque:
            r[..., 3] = 0
        r = np.sign(r) * np.maximum(np.abs(r) - FIDELITY_T, 0)
        if not r.any():
            break
        hd = np.clip(hd + resize_f(r, W, H, Image.BILINEAR), 0, 1)
    return hd


def make_hd(entry, net, scale, cache_dir, floor=PSNR_FLOOR, mip_gamma=1.0):
    import esrgan
    t = entry['t']
    img = gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt)
    alpha = img[..., 3]
    opaque = bool((alpha == 255).all())
    rgb = img[..., :3].astype(np.float32) / 255
    rgb = bleed(rgb, alpha > 0)
    src = np.concatenate([rgb, (alpha.astype(np.float32) / 255)[..., None]], -1)
    mode = (pad_mode(src, 0, t.wrapT), pad_mode(src, 1, t.wrapS))
    cpath = os.path.join(cache_dir, '%016x-%s%s.npz' % (entry['key'], mode[0][0], mode[1][0]))
    if os.path.exists(cpath):
        z = np.load(cpath)
        up_rgb, up_a = z['rgb'], z['a'] if 'a' in z else None
    else:
        up_rgb = esrgan.upscale4(net, rgb, mode=mode)
        up_a = None
        if not opaque:
            a = np.repeat((alpha.astype(np.float32) / 255)[..., None], 3, -1)
            up_a = esrgan.upscale4(net, a, mode=mode).mean(-1, keepdims=True)
        np.savez_compressed(cpath, rgb=up_rgb.astype(np.float16), **({'a': up_a.astype(np.float16)} if up_a is not None else {}))
    up = up_rgb.astype(np.float32) if up_a is None else np.concatenate([up_rgb.astype(np.float32), up_a.astype(np.float32)], -1)
    W, H = t.w * scale, t.h * scale
    hd = np.clip(resize_f(up, W, H, Image.LANCZOS), 0, 1) if scale != 4 else up
    if opaque:
        hd = np.concatenate([hd, np.ones((H, W, 1), np.float32)], -1)
    hd = fidelity(hd, src, alpha > 0, opaque)
    # the plain enlargement of the original: what the network's detail is blended back toward when the compression cannot carry it
    soft = np.clip(resize_f(src, W, H, Image.LANCZOS), 0, 1)
    if opaque:
        soft = np.concatenate([soft[..., :3], np.ones((H, W, 1), np.float32)], -1)
    onoff = (not opaque) and bool(np.isin(alpha, (0, 255)).all())
    r = finish(hd, soft, opaque, onoff, floor, mip_gamma)
    r.update(w=W, h=H, orig=img, opaque=opaque)
    return r


def finish(hd, soft, opaque, onoff, floor=PSNR_FLOOR, mip_gamma=1.0):
    """float RGBA `hd` (and `soft`, the same size) -> encoded levels. The network's share is 1.0, 0.7, 0.4 until level 0 reaches `floor` dB (0: no floor);
    a texture that never does is `rejected` (no levels): the game's own image stays. onoff: the original's alpha is on/off, so is the replacement's
    (a soft rim would leak into transparent frames around the art). Returns dict(fmt, levels, hd (decoded level 0), psnr, psnr_block, psnr_levels,
    blend, rejected)."""
    H, W = hd.shape[:2]
    fmt = PACK_DXT1 if opaque else PACK_DXT5
    best = None
    for blend in BLEND_LADDER:
        cand = hd if blend == 1.0 else np.clip(blend * hd + (1 - blend) * soft, 0, 1)
        if onoff:
            cand = cand.copy()
            cand[..., 3] = (cand[..., 3] >= 0.5).astype(np.float32)
        lv0 = (np.clip(cand, 0, 1) * 255 + 0.5).astype(np.uint8)
        if opaque:
            lv0[..., 3] = 255
        blob, dec = encode(lv0, W, H, fmt)
        p = hdtex_model.psnr_rgb(lv0, dec)
        if best is None or p > best[0]:
            best = (p, blend, cand, lv0, blob, dec)
        if p >= floor:
            break
    p, blend, cand, lv0, blob, dec = best
    out = dict(fmt=fmt, hd=dec, psnr=p, psnr_block=hdtex_model.worst_block_psnr(lv0, dec), blend=blend, rejected=None, levels=[], psnr_levels=[p])
    if floor and p < floor:
        out['rejected'] = 'PSNR %.1f dB under the %.0f dB floor at every blend' % (p, floor)
        return out
    if opaque:
        cand = np.concatenate([cand[..., :3], np.ones(cand.shape[:2] + (1,), np.float32)], -1)
    chain = hdtex_model.make_mips(cand, gamma=mip_gamma)
    out['levels'].append(blob)
    for lv in chain[1:]:
        lh, lw = lv.shape[:2]
        lv8 = (np.clip(lv, 0, 1) * 255 + 0.5).astype(np.uint8)
        if opaque:
            lv8[..., 3] = 255
        b, d = encode(lv8, lw, lh, fmt)
        out['levels'].append(b)
        if min(lw, lh) >= 8:
            out['psnr_levels'].append(hdtex_model.psnr_rgb(lv8, d))
    return out


def write_pack(path, items):
    """items: [(key, entry, hd)]; layout documented in README.md. The index is sorted by key; the data is written in the order of the selection
    (entry['prio'], the line of selection.txt: Mario and Luigi first), so that a reader that keeps only the first part of a large pack in memory
    (GXV_HDTEX_PRELOAD) holds the textures that are used most."""
    items = sorted(items, key=lambda x: x[0])
    names = bytearray()
    name_off = []
    for k, e, hd in items:
        name_off.append(len(names))
        names += ' '.join(e['names']).encode('ascii', 'replace')[:200] + b'\0'
    n = len(items)
    index_off = 32
    names_off = index_off + 32 * n
    data_off = (names_off + len(names) + 63) // 64 * 64
    blobs = bytearray()
    offs = {}
    for k, e, hd in sorted(items, key=lambda x: (x[1].get('prio', 0), x[1]['names'][0], x[0])):
        while (data_off + len(blobs)) % 64:
            blobs.append(0)
        offs[k] = data_off + len(blobs)
        blobs += b''.join(hd['levels'])
    index = bytearray()
    for (k, e, hd), no in zip(items, name_off):
        t = e['t']
        flags = 0 if hd['opaque'] else 1
        index += struct.pack('<QIIHHHHBBBBI', k, offs[k], sum(len(b) for b in hd['levels']), hd['w'], hd['h'], t.w, t.h, hd['fmt'], len(hd['levels']), t.fmt, flags, no)
    size = data_off + len(blobs)
    hdr = struct.pack('<4sIIIIIII', b'HDTX', 1, n, index_off, names_off, data_off, size, 0)
    with open(path + '.tmp', 'wb') as f:
        f.write(hdr)
        f.write(index)
        f.write(names)
        f.write(b'\0' * (data_off - names_off - len(names)))
        f.write(blobs)
    os.replace(path + '.tmp', path)
    return size


def sheet(path, rows, S=256):
    """Before (original, nearest-neighbour to the same size) / after (the BC-compressed HD level 0)."""
    cols = 2
    pairs = 3
    nrows = (len(rows) + pairs - 1) // pairs
    im = Image.new('RGB', (pairs * cols * S, nrows * (S + 14)), (32, 32, 32))
    dr = ImageDraw.Draw(im)
    for i, (name, orig, hd) in enumerate(rows):
        x0, y0 = (i % pairs) * cols * S, (i // pairs) * (S + 14)
        dr.text((x0 + 2, y0 + 1), name[:60], fill=(255, 255, 0))
        sc = S / max(hd.shape[:2])
        size = (max(1, int(hd.shape[1] * sc)), max(1, int(hd.shape[0] * sc)))
        for c, a in enumerate((orig, hd)):
            p = Image.fromarray(a, 'RGBA').resize(size, Image.NEAREST if c == 0 else Image.LANCZOS)
            chk = (np.indices((p.size[1], p.size[0])).sum(0) // 8 % 2)[..., None]
            bg = np.where(chk, 150, 110).astype(np.uint8).repeat(3, -1)
            bg = np.concatenate([bg, np.full(bg.shape[:2] + (1,), 255, np.uint8)], -1)
            comp = Image.alpha_composite(Image.fromarray(bg, 'RGBA'), p).convert('RGB')
            im.paste(comp, (x0 + c * S, y0 + 14))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA, help='the files/ folder of your prepared (cooked, flat) game data')
    ap.add_argument('--out', default='hdtex-build')
    ap.add_argument('--model', default=None, help='default: <out>/models/realesr-general-x4v3.pth')
    ap.add_argument('--selection', default=os.path.join(HERE, 'selection.txt'))
    ap.add_argument('--scale', type=int, default=2, choices=(2, 4))
    ap.add_argument('--threads', type=int, default=2)
    ap.add_argument('--psnr-floor', type=float, default=PSNR_FLOOR,
                    help='compression PSNR (dB) a texture must reach to go in (default %.0f; 0: none): below it the network\'s share is lowered, then the texture is left out' % PSNR_FLOOR)
    ap.add_argument('--mip-gamma', type=float, default=1.0, help='mip levels are averaged in encoded values (1, the game\'s own way) or in linear light (2.2)')
    ap.add_argument('--list', action='store_true', help='only list the selected textures (with their size on screen verdicts)')
    ap.add_argument('--report', metavar='PACK', help='print what a finished pack costs and how much of it the picture can use (no game data needed), then stop')
    ap.add_argument('--manifest', default=None, help='with --report: the build\'s manifest.tsv, for the PSNR figures')
    ap.add_argument('--list-all', action='store_true', help='ignore selection.txt: every eligible texture of ObjectData/ and StageData/, as TSV '
                    '(key, fmt, w, h, alpha, hd bytes, names): what tools/hdtex/select_from_census.py reads')
    ap.add_argument('--select-keys', default=None, help='a census selection file (keys and/or archive:texture lines): build exactly those, searched in '
                    'every archive (selection.txt is not used)')
    a = ap.parse_args()
    if a.report:
        res = hdtex_model.report(a.report, manifest=a.manifest, floor=a.psnr_floor)
        sys.exit(1 if res.get('problems') else 0)
    if a.model is None:
        a.model = os.path.join(a.out, 'models', 'realesr-general-x4v3.pth')
    if a.list_all or a.select_keys:
        texs = collect(a.data, all_archives(a.data), read_keylist(a.select_keys) if a.select_keys else None)
    else:
        texs = collect(a.data, read_selection(a.selection))
    if a.list_all:
        print('# key\tfmt\tw\th\talpha\thd_bytes_2x\tnames')
        for k, e in sorted(texs.items(), key=lambda kv: kv[1]['names'][0]):
            t = e['t']
            alpha = int(not bool((gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt)[..., 3] == 255).all()))
            print('%016x\t%s\t%d\t%d\t%d\t%d\t%s' % (k, gxarc.FMT_NAME[t.fmt], t.w, t.h, alpha, pack_bytes(t.w * 2, t.h * 2, alpha), ' '.join(e['names'])))
        return
    print('%d textures selected' % len(texs))
    table = hdtex_model.read_screensize()
    if a.list:
        tot = 0
        print('%-16s %-6s %-9s %9s  %-15s %s' % ('key', 'format', 'size', 'est. KiB', 'screen', 'names (est.: the 2x replacement as DXT1 with its mip chain; DXT5 is twice that)'))
        for k, e in sorted(texs.items(), key=lambda kv: kv[1]['names'][0]):
            t = e['t']
            est = (t.w * a.scale) * (t.h * a.scale) // 2 * 4 // 3
            tot += est
            d, ty, cl, v = hdtex_model.screen_note(e['names'], t.w, t.h, table)
            print('%016x %-6s %4dx%-4d %9.0f  %-15s %s' % (k, gxarc.FMT_NAME[t.fmt], t.w, t.h, est / 1024.0, v, ' '.join(e['names'])))
        print('about %.2f MiB of GPU memory if all were alive at once (%d textures)' % (tot / 1048576.0, len(texs)))
        return
    import torch
    import esrgan
    torch.set_num_threads(a.threads)
    net = esrgan.load(a.model)
    cache = os.path.join(a.out, 'cache')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(os.path.join(a.out, 'sheets'), exist_ok=True)
    items, rows, left_out = [], [], []
    t0 = time.time()
    for i, (k, e) in enumerate(sorted(texs.items(), key=lambda kv: kv[1]['names'][0])):
        hd = make_hd(e, net, a.scale, cache, a.psnr_floor, a.mip_gamma)
        e['hd'] = hd
        rows.append((e['names'][0], hd['orig'], hd['hd']))
        if hd['rejected']:
            left_out.append((k, e))
        else:
            items.append((k, e, hd))
        print('[%3d/%d %5.0f s] %016x %-40s %4dx%-4d -> %4dx%-4d %s psnr %.1f (worst block %.1f)%s%s' % (
            i + 1, len(texs), time.time() - t0, k, e['names'][0][:40], e['t'].w, e['t'].h, hd['w'], hd['h'], 'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5',
            hd['psnr'], hd['psnr_block'], '' if hd['blend'] == 1.0 else ', network share %.0f %%' % (100 * hd['blend']),
            ' LEFT OUT: ' + hd['rejected'] if hd['rejected'] else ''), flush=True)
    pack = os.path.join(a.out, 'hdtex.pack')
    size = write_pack(pack, items)
    with open(os.path.join(a.out, 'manifest.tsv'), 'w') as f:
        f.write('key\tnames\torig_fmt\torig_w\torig_h\thd_fmt\thd_w\thd_h\tlevels\tbytes\torig_bytes\tpsnr_db\tpsnr_block_db\tpsnr_mip_min_db\tnetwork_share\t'
                'gpu_bytes_console\tdetail_px\ttypical_px\tcloseup_px\tscreen\tstatus\n')
        for k, e in sorted(texs.items(), key=lambda kv: kv[1]['names'][0]):
            t, hd = e['t'], e['hd']
            ls = hdtex_model.level_sizes(hd['w'], hd['h'], max(1, len(hd['levels'])), hd['fmt'])
            con = sum(x[2] for x in ls[:hdtex_model.console_levels(hd['w'], hd['h'], len(ls))])
            d, ty, cl, v = hdtex_model.screen_note(e['names'], t.w, t.h, table)
            f.write('%016x\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%.0f\t%s\t%s\t%s\t%s\n' % (
                k, ' '.join(e['names']), gxarc.FMT_NAME[t.fmt], t.w, t.h, 'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5', hd['w'], hd['h'], len(hd['levels']),
                sum(len(b) for b in hd['levels']), len(t.level0), hd['psnr'], hd['psnr_block'], min(hd['psnr_levels']), hd['blend'] * 100, con, d,
                '' if ty is None else ty, '' if cl is None else cl, v, 'left out: ' + hd['rejected'] if hd['rejected'] else 'in the pack'))
    per = 18
    for s in range(0, len(rows), per):
        sheet(os.path.join(a.out, 'sheets', 'sheet%02d.png' % (s // per)), rows[s:s + per])
    print('pack: %s, %d textures, %.2f MiB (%.0f s)%s' % (pack, len(items), size / 1048576.0, time.time() - t0,
                                                         '; %d left out under the PSNR floor' % len(left_out) if left_out else ''))
    hdtex_model.report(pack, manifest=os.path.join(a.out, 'manifest.tsv'), floor=a.psnr_floor)


if __name__ == '__main__':
    main()

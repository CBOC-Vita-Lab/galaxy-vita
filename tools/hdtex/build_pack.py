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
mip chain by area averaging down to 1x1 -> BC1 (opaque) or BC3 (any alpha below 255).
Needs: numpy, Pillow, xxhash, torch (CPU), quicktex.
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

HERE = os.path.dirname(os.path.abspath(__file__))
COLOUR_FORMATS = {0x4, 0x5, 0x6, 0x8, 0x9, 0xA, 0xE}
EXCLUDE_NAMES = re.compile(r'(?i)(env|high_?light|hilight|spec|noise|noizu|^ind|ind$|testind|dummy|mask|fooline)')
MIN_DIM = 16
PACK_DXT1, PACK_DXT5 = 1, 3


def tex_key(fmt, w, h, level0, palette):
    """Must match HdKey() in platform/vgx/src/vgx_hdtex.cpp."""
    seed = fmt | (w << 8) | (h << 20)
    k = xxhash.xxh3_64_intdigest(level0, seed=seed)
    if palette:
        k = xxhash.xxh3_64_intdigest(palette, seed=k)
    return k


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


def collect(data_root, selection):
    """{key: dict} of the selected textures, deduplicated by content."""
    texs = {}
    for arc, rx in selection:
        path = os.path.join(data_root, 'ObjectData', arc + '.arc')
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
                if k in texs:
                    if '%s:%s' % (arc, t.name) not in texs[k]['names']:
                        texs[k]['names'].append('%s:%s' % (arc, t.name))
                    continue
                texs[k] = dict(key=k, names=['%s:%s' % (arc, t.name)], t=t)
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


def make_hd(entry, net, scale, cache_dir):
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
    if not opaque and np.isin(alpha, (0, 255)).all():
        # The original's alpha is on/off (CMPR): so is the replacement's, with the network's
        # smoother outline (a soft rim would leak into transparent frames around the art).
        hd[..., 3] = (hd[..., 3] >= 0.5).astype(np.float32)
    fmt = PACK_DXT1 if opaque else PACK_DXT5
    levels, decoded = [], []
    lw, lh = W, H
    while True:
        lv = hd if (lw, lh) == (W, H) else resize_f(hd, lw, lh, Image.BOX)
        lv8 = (np.clip(lv, 0, 1) * 255 + 0.5).astype(np.uint8)
        if opaque:
            lv8[..., 3] = 255
        blob, dec = encode(lv8, lw, lh, fmt)
        levels.append(blob)
        decoded.append((lv8, dec))
        if lw == 1 and lh == 1:
            break
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    src8, dec0 = decoded[0]
    mse = np.mean((src8.astype(np.float32) - dec0.astype(np.float32)) ** 2)
    psnr = 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    return dict(fmt=fmt, w=W, h=H, levels=levels, orig=img, hd=dec0, psnr=psnr, opaque=opaque)


def write_pack(path, items):
    """items: [(key, entry, hd)]; layout documented in README.md."""
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
    index = bytearray()
    for (k, e, hd), no in zip(items, name_off):
        while (data_off + len(blobs)) % 64:
            blobs.append(0)
        off = data_off + len(blobs)
        data = b''.join(hd['levels'])
        blobs += data
        t = e['t']
        flags = 0 if hd['opaque'] else 1
        index += struct.pack('<QIIHHHHBBBBI', k, off, len(data), hd['w'], hd['h'], t.w, t.h, hd['fmt'], len(hd['levels']), t.fmt, flags, no)
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
    ap.add_argument('--list', action='store_true', help='only list the selected textures')
    a = ap.parse_args()
    if a.model is None:
        a.model = os.path.join(a.out, 'models', 'realesr-general-x4v3.pth')
    texs = collect(a.data, read_selection(a.selection))
    print('%d textures selected' % len(texs))
    if a.list:
        for k, e in sorted(texs.items(), key=lambda kv: kv[1]['names'][0]):
            t = e['t']
            print('%016x %-6s %4dx%-4d %s' % (k, gxarc.FMT_NAME[t.fmt], t.w, t.h, ' '.join(e['names'])))
        return
    import torch
    import esrgan
    torch.set_num_threads(a.threads)
    net = esrgan.load(a.model)
    cache = os.path.join(a.out, 'cache')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(os.path.join(a.out, 'sheets'), exist_ok=True)
    items, rows = [], []
    t0 = time.time()
    for i, (k, e) in enumerate(sorted(texs.items(), key=lambda kv: kv[1]['names'][0])):
        hd = make_hd(e, net, a.scale, cache)
        items.append((k, e, hd))
        rows.append((e['names'][0], hd['orig'], hd['hd']))
        print('[%3d/%d %5.0f s] %016x %-40s %4dx%-4d -> %4dx%-4d %s psnr %.1f' % (i + 1, len(texs), time.time() - t0, k, e['names'][0][:40], e['t'].w, e['t'].h,
                                                                              hd['w'], hd['h'], 'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5', hd['psnr']), flush=True)
    size = write_pack(os.path.join(a.out, 'hdtex.pack'), items)
    with open(os.path.join(a.out, 'manifest.tsv'), 'w') as f:
        f.write('key\tnames\torig_fmt\torig_w\torig_h\thd_fmt\thd_w\thd_h\tlevels\tbytes\torig_bytes\tpsnr_db\n')
        for k, e, hd in sorted(items, key=lambda x: x[1]['names'][0]):
            t = e['t']
            f.write('%016x\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%.1f\n' % (k, ' '.join(e['names']), gxarc.FMT_NAME[t.fmt], t.w, t.h,
                                                                             'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5', hd['w'], hd['h'],
                                                                             len(hd['levels']), sum(len(b) for b in hd['levels']), len(t.level0), hd['psnr']))
    per = 18
    for s in range(0, len(rows), per):
        sheet(os.path.join(a.out, 'sheets', 'sheet%02d.png' % (s // per)), rows[s:s + per])
    print('pack: %s, %d textures, %.2f MiB (%.0f s)' % (os.path.join(a.out, 'hdtex.pack'), len(items), size / 1048576.0, time.time() - t0))


if __name__ == '__main__':
    main()

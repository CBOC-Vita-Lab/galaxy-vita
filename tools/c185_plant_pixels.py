"""Private-data pixel fixture, not a runtime screenshot or a comparison of frame times.

Project the cooked meshes and their actual cutout textures onto a flat, seven-member
PlantGroup ring at fixed test orientations. Compare the largest newly covered area in
two native pictures, and require exact recovery of the whole group's pixels. This
does not reproduce in-game camera angles, animation, lighting or depth occluders.
"""
import argparse
from pathlib import Path
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent / 'hdtex'))
import gxarc
import gxdecode
from farlod_extract import shapes


def triangles(model, name):
    for raw in shapes(model, name):
        _flags, nv, ni = struct.unpack_from('<III', raw, 48)
        verts = np.frombuffer(raw, dtype='<f4', count=nv * 12, offset=60).reshape((-1, 12))
        ids = np.frombuffer(raw, dtype='<u4', count=ni, offset=60+48*nv).reshape((-1, 3))
        yield from verts[ids]


def sample(uv, mode, size):
    t = np.floor(uv * size).astype(int)
    if mode == 0:
        return t.clip(0, size - 1)
    if mode == 2:
        t %= 2 * size
        return np.where(t < size, t, 2 * size - 1 - t)
    return t % size


def picture(mesh, tex, mask):
    # Authored group layout: one in the middle, then six at radius 160.
    rgba = gxdecode.decode(tex.fmt, tex.w, tex.h, tex.level0, tex.palette, tex.palFmt)
    image = np.zeros((108, 156, 4), dtype=np.uint8)
    zbuf = np.full(image.shape[:2], -np.inf)
    tilt = np.radians(30)
    for i in range(7):
        if not (mask & (1 << i)):
            continue
        theta = (i - 1) * np.pi / 3
        ox, oz = (160*np.cos(theta), 160*np.sin(theta)) if i else (0, 0)
        yaw = 0.7 + i * 1.7
        for t in mesh:
            x = t[:, 0]*np.cos(yaw) + t[:, 2]*np.sin(yaw) + ox
            z = -t[:, 0]*np.sin(yaw) + t[:, 2]*np.cos(yaw) + oz
            y = t[:, 1]
            depth = y*np.sin(tilt) + z*np.cos(tilt)
            x = 77 + .27*x
            y = 67 - .27*(y*np.cos(tilt) - z*np.sin(tilt))
            lo_x, hi_x = max(0, int(np.floor(x.min()))), min(155, int(np.ceil(x.max())))
            lo_y, hi_y = max(0, int(np.floor(y.min()))), min(107, int(np.ceil(y.max())))
            if hi_x < lo_x or hi_y < lo_y:
                continue
            yy, xx = np.mgrid[lo_y:hi_y+1, lo_x:hi_x+1] + 0.5
            det = (y[1]-y[2])*(x[0]-x[2]) + (x[2]-x[1])*(y[0]-y[2])
            if abs(det) < .001:
                continue
            a = ((y[1]-y[2])*(xx-x[2]) + (x[2]-x[1])*(yy-y[2])) / det
            b = ((y[2]-y[0])*(xx-x[2]) + (x[0]-x[2])*(yy-y[2])) / det
            c = 1-a-b
            def mix(vals):
                return a*vals[0] + b*vals[1] + c*vals[2]
            pixels = rgba[sample(mix(t[:, 11]), tex.wrapT, tex.h), sample(mix(t[:, 10]), tex.wrapS, tex.w)]
            rect = np.s_[lo_y:hi_y+1, lo_x:hi_x+1]
            d = mix(depth)
            valid = (a>=0) & (b>=0) & (c>=0) & (d>zbuf[rect]) & (pixels[:, :, 3] == 255)
            image[rect][valid] = pixels[valid]
            zbuf[rect][valid] = d[valid]
    return image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('object_dir', type=Path)
    p.add_argument('behaviour', type=Path)
    p.add_argument('out_png', type=Path, nargs='?')
    args = p.parse_args()
    m = re.search(r'control=00/7f/7f candidate=00/([0-9a-f]+)/7f', args.behaviour.read_text())
    assert m
    mask = int(m[1], 16)
    rows = []
    for name in ('CutBush', 'FlowerBlue'):
        arc, = args.object_dir.rglob(name+'.arc')
        mdl, = (data for f, data in gxarc.rarc_files(arc.read_bytes()) if f.endswith('.bdl'))
        tex, = gxarc.model_textures(mdl)
        mesh = list(triangles(mdl, name))
        base = picture(mesh, tex, 0x7f)
        phase1, phase2 = picture(mesh, tex, mask), picture(mesh, tex, 0x7f)
        assert np.array_equal(base, phase2)
        old = base[:, :, 3] != 0
        first = phase1[:, :, 3] != 0
        assert np.all(~first | old)
        n = int(old.sum())
        d1, d2 = int(first.sum()), int((old & ~first).sum())
        assert n > 100 and 0 < max(d1, d2) <= 0.80 * n and d1+d2 == n
        print(f'{name}: {len(mesh)} cooked triangles/member; newly covered pixels control {n}+0, '
              f'candidate {d1}+{d2}; picture 2 identical RGBA')
        rows.extend([(name+' / control', (None, base, base)), (name+' / C185', (None, phase1, phase2))])
    if args.out_png:
        out = Image.new('RGB', (584, 480), '#283035')
        dr = ImageDraw.Draw(out)
        for j, (label, imgs) in enumerate(rows):
            dr.text((4, 58+j*112), label, fill='white')
            for i, arr in enumerate(imgs):
                if arr is not None:
                    frame = Image.fromarray(arr)
                    out.paste(frame, (108+i*158, 24+j*112), frame)
                if j == 0:
                    dr.text((111+i*158, 8), ['Clipped', 'Native picture 1', 'Native picture 2'][i], fill='white')
        out.save(args.out_png)


if __name__ == '__main__':
    main()

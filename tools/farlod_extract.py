#!/usr/bin/env python3
"""farlod32: the triangles of a cooked (little-endian) J3D model, per shape packet as the port's display-list cache sees it
(one welded vertex block and one triangle list per matrix group's display list), for tests/vgx_farlod.cpp --mesh.

  python3 -I tools/farlod_extract.py out.fld ObjectData/Foo.arc [more.arc ...]

Reads the archives only (never writes beside them). Output: "FLD1", u32 shapes; per shape: char name[48], u32 flags
(1 skinned = has a per-vertex matrix index, 2 normals, 4 colours, 8 texture coordinates; bits 8-10: the material's GX cull mode + 1
[1 none, 2 front, 3 back, 4 all], 0 not known), u32 nv, u32 ni, nv x 12 f32
(position 3, normal 3, colour 4 in 0..1, tex0 2), ni x u32 (triangles)."""
import struct
import sys
import os

COMP = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}


def models(data):
    for magic in (b'2D3J4ldb', b'2D3J3dmb'):
        at = 0
        while True:
            at = data.find(magic, at)
            if at < 0:
                break
            size = struct.unpack_from('<I', data, at + 8)[0]
            yield data[at:at + size]
            at += 8


def blocks(b):
    n = struct.unpack_from('<I', b, 0x0C)[0]
    o = 0x20
    out = {}
    for _ in range(n):
        if o + 8 > len(b):
            break
        tag = bytes(b[o:o + 4])[::-1]
        size = struct.unpack_from('<I', b, o + 4)[0]
        out[tag] = (o, size)
        if size <= 0:
            break
        o += size
    return out


def read_vtx(b, o, size):
    names = ['fmt', 'pos', 'nrm', 'nbt', 'clr0', 'clr1'] + ['tex%d' % i for i in range(8)]
    offs = {n: struct.unpack_from('<I', b, o + 8 + 4 * i)[0] for i, n in enumerate(names)}
    fm = {}
    p = o + offs['fmt']
    while True:
        attr, cnt, typ = struct.unpack_from('<III', b, p)
        if attr == 0xFF:
            break
        fm[attr] = (cnt, typ, b[p + 12])
        p += 16
    return offs, fm


def shapes(b, name):
    bl = blocks(b)
    if b'VTX1' not in bl or b'SHP1' not in bl:
        return
    vo, vs = bl[b'VTX1']
    offs, fm = read_vtx(b, vo, vs)
    so, ss = bl[b'SHP1']
    num = struct.unpack_from('<H', b, so + 8)[0]
    names = ['init', 'index', 'name', 'vtxDesc', 'mtxTable', 'dl', 'mtxInit', 'drawInit']
    so_ = {n: struct.unpack_from('<I', b, so + 0x0C + 4 * i)[0] for i, n in enumerate(names)}

    # the cull mode of each shape's material (INF1 hierarchy: a material node, then its shapes; MAT3: init data byte 1 = cull table index)
    cull_of = {}
    if b'INF1' in bl and b'MAT3' in bl:
        io, isz = bl[b'INF1']
        mo, msz = bl[b'MAT3']
        m_init, m_id = struct.unpack_from('<II', b, mo + 0x0C)
        m_cull = struct.unpack_from('<I', b, mo + 0x0C + 16)[0]
        h = io + struct.unpack_from('<I', b, io + 0x14)[0]
        mat = None
        while h + 4 <= io + isz:
            typ, idx = struct.unpack_from('<HH', b, h)
            h += 4
            if typ == 0:
                break
            if typ == 0x11:
                mat = idx
            elif typ == 0x12 and mat is not None:
                rec_i = struct.unpack_from('<H', b, mo + m_id + 2 * mat)[0]
                ci = b[mo + m_init + rec_i * 0x14C + 1]
                cull_of[idx] = struct.unpack_from('<I', b, mo + m_cull + 4 * ci)[0] & 3

    def pos(i):
        cnt, typ, frac = fm[9]
        c, w = COMP[typ]
        v = struct.unpack_from('<3' + c, b, vo + offs['pos'] + i * 3 * w)
        return v if typ == 4 else tuple(x / float(1 << frac) for x in v)

    def nrm(i):
        cnt, typ, frac = fm[10]
        c, w = COMP[typ]
        v = struct.unpack_from('<3' + c, b, vo + offs['nrm'] + i * 3 * w)
        if typ == 4:
            return v
        return tuple(x / float(1 << (14 if typ == 3 else 6)) for x in v)

    def clr(i):
        cnt, typ, frac = fm[11]
        base = vo + offs['clr0']
        if typ == 5:
            return tuple(x / 255.0 for x in b[base + 4 * i: base + 4 * i + 4])
        if typ in (1, 2):
            w = 3 if typ == 1 else 4
            q = b[base + w * i: base + w * i + 3]
            return (q[0] / 255.0, q[1] / 255.0, q[2] / 255.0, 1.0)
        if typ == 0:
            v = struct.unpack_from('<H', b, base + 2 * i)[0]
            return ((v >> 11) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0, 1.0)
        if typ == 3:
            v = struct.unpack_from('<H', b, base + 2 * i)[0]
            return ((v >> 12) / 15.0, ((v >> 8) & 15) / 15.0, ((v >> 4) & 15) / 15.0, (v & 15) / 15.0)
        return (1.0, 1.0, 1.0, 1.0)

    def tex(i):
        cnt, typ, frac = fm[13]
        c, w = COMP[typ]
        n = 2 if cnt == 1 else 1
        v = struct.unpack_from('<%d%s' % (n, c), b, vo + offs['tex0'] + i * n * w)
        if typ != 4:
            v = tuple(x / float(1 << frac) for x in v)
        return (v[0], v[1] if n == 2 else 0.0)

    for s in range(num):
        rec = so + so_['init'] + s * 0x28
        mtx_groups, desc_off, mtx_init, draw_init = struct.unpack_from('<4H', b, rec + 2)
        desc = []
        p = so + so_['vtxDesc'] + desc_off
        while True:
            attr, typ = struct.unpack_from('<II', b, p)
            if attr == 0xFF:
                break
            desc.append((attr, typ))
            p += 8
        skinned = any(a == 0 for a, t in desc)
        flags = (((cull_of.get(s, 0) + 1) << 8) if s in cull_of else 0) | (1 if skinned else 0) | (2 if any(a == 10 for a, t in desc) else 0) | (4 if any(a == 11 for a, t in desc) else 0) | (8 if any(a == 13 for a, t in desc) else 0)
        for g in range(mtx_groups):
            dsize, doff = struct.unpack_from('<II', b, so + so_['drawInit'] + (draw_init + g) * 8)
            dl = b[so + so_['dl'] + doff: so + so_['dl'] + doff + dsize]
            verts = {}
            vlist = []
            tris = []
            q = 0
            ok = True
            while q < len(dl):
                op = dl[q]
                if op == 0:
                    q += 1
                    continue
                prim = op & 0xF8
                if prim not in (0x80, 0x90, 0x98, 0xA0):
                    ok = prim in (0xA8, 0xB0, 0xB8)
                    break
                cnt = struct.unpack_from('>H', dl, q + 1)[0]
                q += 3
                ids = []
                for _ in range(cnt):
                    key = []
                    for attr, typ in desc:
                        if typ == 1:
                            key.append(dl[q])
                            q += 1
                        elif typ == 2:
                            key.append(dl[q])
                            q += 1
                        elif typ == 3:
                            key.append(struct.unpack_from('>H', dl, q)[0])
                            q += 2
                    key = tuple(key)
                    if key not in verts:
                        verts[key] = len(vlist)
                        vlist.append(key)
                    ids.append(verts[key])
                if prim == 0x90:
                    for i in range(0, cnt - 2, 3):
                        tris += [ids[i], ids[i + 1], ids[i + 2]]
                elif prim == 0x98:
                    for i in range(cnt - 2):
                        tris += [ids[i], ids[i + 1], ids[i + 2]] if i % 2 == 0 else [ids[i + 1], ids[i], ids[i + 2]]
                elif prim == 0xA0:
                    for i in range(1, cnt - 1):
                        tris += [ids[0], ids[i], ids[i + 1]]
                else:
                    for i in range(0, cnt - 3, 4):
                        tris += [ids[i], ids[i + 1], ids[i + 2], ids[i], ids[i + 2], ids[i + 3]]
            if not ok or not tris:
                continue
            out = bytearray()
            for key in vlist:
                d = dict(zip([a for a, t in desc], key))
                p3 = pos(d[9])
                n3 = nrm(d[10]) if 10 in d and 10 in fm else (0.0, 0.0, 0.0)
                c4 = clr(d[11]) if 11 in d and 11 in fm else (1.0, 1.0, 1.0, 1.0)
                t2 = tex(d[13]) if 13 in d and 13 in fm else (0.0, 0.0)
                out += struct.pack('<12f', *(tuple(p3) + tuple(n3) + tuple(c4) + tuple(t2)))
            label = ('%s#%d.%d' % (name, s, g)).encode()[:47]
            yield label.ljust(48, b'\0') + struct.pack('<III', flags, len(vlist), len(tris)) + bytes(out) + struct.pack('<%dI' % len(tris), *tris)


def main():
    out = sys.argv[1]
    recs = []
    for path in sys.argv[2:]:
        data = open(path, 'rb').read()
        base = os.path.basename(path).replace('.arc', '')
        for m in models(data):
            try:
                recs += list(shapes(m, base))
            except (struct.error, KeyError, IndexError) as e:
                sys.stderr.write('%s: skipped a model (%r)\n' % (path, e))
    with open(out, 'wb') as f:
        f.write(b'FLD1' + struct.pack('<I', len(recs)))
        for r in recs:
            f.write(r)
    sys.stderr.write('%s: %d shape packets\n' % (out, len(recs)))


if __name__ == '__main__':
    main()

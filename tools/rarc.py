#!/usr/bin/env python3
"""Minimal Yaz0 + RARC reader used for surveying and cooking game data."""
import ctypes
import os
import struct

_dll = None
_dll_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cook', 'native', 'yaz0.dll')
if os.path.exists(_dll_path):
    _dll = ctypes.CDLL(_dll_path)
    _dll.yaz0_decode.restype = ctypes.c_longlong
    _dll.yaz0_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]


def yaz0_decompress(data):
    if data[:4] != b'Yaz0':
        return data
    if _dll is not None:
        size = struct.unpack_from('>I', data, 4)[0]
        out = ctypes.create_string_buffer(size)
        n = _dll.yaz0_decode(bytes(data), len(data), out, size)
        if n != size:
            raise ValueError('bad Yaz0 stream')
        return out.raw
    size = struct.unpack_from('>I', data, 4)[0]
    out = bytearray(size)
    src = 16
    dst = 0
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
                b1 = data[src]
                b2 = data[src + 1]
                src += 2
                dist = ((b1 & 0x0F) << 8) | b2
                copy_src = dst - dist - 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                for _ in range(n):
                    out[dst] = out[copy_src]
                    dst += 1
                    copy_src += 1
    return bytes(out)


class RarcFile:
    def __init__(self, name, data, is_dir=False):
        self.name = name
        self.data = data
        self.is_dir = is_dir


def rarc_list(data):  # generator
    """Yields (path, bytes) for every file in a RARC archive."""
    if data[:4] == b'CRAR':
        yield from crar_list(data)
        return
    if data[:4] == b'Yaz0':
        data = yaz0_decompress(data)
    if data[:4] != b'RARC':
        raise ValueError('not RARC')
    data_off = struct.unpack_from('>I', data, 0x0C)[0] + 0x20
    info = 0x20
    num_nodes, node_off, num_entries, entry_off, str_size, str_off = struct.unpack_from('>IIIIII', data, info)
    node_off += 0x20
    entry_off += 0x20
    str_off += 0x20

    def cstr(off):
        end = data.index(b'\0', str_off + off)
        return data[str_off + off:end].decode('shift_jis', errors='replace')

    nodes = []
    for i in range(num_nodes):
        ident, name_ofs, name_hash, n_ent, first = struct.unpack_from('>IIHHI', data, node_off + i * 0x10)
        nodes.append((cstr(name_ofs), n_ent, first))

    def walk(node_idx, prefix):
        name, n_ent, first = nodes[node_idx]
        for e in range(first, first + n_ent):
            fid, h, flags_type, name_ofs, d_off, d_size = struct.unpack_from('>HHHHII', data, entry_off + e * 0x14)
            ename = cstr(name_ofs)
            ftype = flags_type >> 8
            if ftype & 0x02:  # directory
                if ename in ('.', '..'):
                    continue
                yield from walk(d_off, prefix + ename + '/')
            else:
                yield prefix + ename, data[data_off + d_off:data_off + d_off + d_size]

    yield from walk(0, nodes[0][0] + '/')


def crar_list(b):
    """Yields (path, bytes) for every file of a cooked RARC ('CRAR'): the port's
    little-endian copy of RARC written by tools/cook (same tables, header fields swapped;
    see tools/hdtex/gxarc.py, which this mirrors)."""
    if b[:4] != b'CRAR':
        raise ValueError('not a cooked RARC')
    u16 = lambda o: struct.unpack_from('<H', b, o)[0]
    u32 = lambda o: struct.unpack_from('<I', b, o)[0]
    hdr = u32(8)
    data_start = hdr + u32(0xC)
    ndirs, dir_off, nfiles, file_off, _, str_off = (u32(hdr + 4 * i) for i in range(6))
    dir_off += hdr
    file_off += hdr
    str_off += hdr

    def cstr(o):
        e = b.index(b'\0', str_off + o)
        return bytes(b[str_off + o:e]).decode('shift_jis', errors='replace')
    dirs = [(cstr(u32(dir_off + 0x10 * i + 4)), u16(dir_off + 0x10 * i + 0xA), u32(dir_off + 0x10 * i + 0xC))
            for i in range(ndirs)]

    def walk(di, prefix, depth):
        if depth > 32:
            return
        name, count, first = dirs[di]
        for e in range(first, first + count):
            f = file_off + 0x14 * e
            word = u32(f + 4)
            flags, name_off = word >> 24, word & 0xFFFFFF
            en = cstr(name_off)
            if flags & 0x02:
                if en in ('.', '..'):
                    continue
                yield from walk(u32(f + 8), prefix + en + '/', depth + 1)
            else:
                o = data_start + u32(f + 8)
                yield prefix + en, bytes(b[o:o + u32(f + 0xC)])
    yield from walk(0, dirs[0][0] + '/', 0)


def open_archive(path):
    """[(path, bytes)] of a RARC, Yaz0-compressed RARC or cooked CRAR file."""
    with open(path, 'rb') as f:
        return list(rarc_list(f.read()))


def bcsv_rows(data, big_endian=None):
    """Decode a JMap BCSV (big-endian on disc, little-endian once cooked) into
    (fields, rows): fields = [(hash, type)], rows = list of {hash: value}.
    Strings come back as str. Field names are CRC-less hashes: use JMap name hashes
    (Game/Util/JMapInfo) to read them; bcsv_hash() below computes the game's hash."""
    for e in (('>', '<') if big_endian is None else (('>',) if big_endian else ('<',))):
        n, items, off, esize = struct.unpack_from(e + 'iiiI', data, 0)
        if 0 <= n < 1000000 and 0 <= items <= 512 and off == 16 + items * 12 and off + n * esize <= len(data):
            break
    else:
        raise ValueError('not a BCSV')
    fields = []
    for i in range(items):
        h, mask, offs, shift, typ = struct.unpack_from(e + 'IIHBB', data, 16 + i * 12)
        fields.append((h, mask, offs, shift, typ))
    sbase = off + n * esize
    rows = []
    for r in range(n):
        base = off + r * esize
        row = {}
        for h, mask, offs, shift, typ in fields:
            p = base + offs
            if typ in (0, 3):
                v = struct.unpack_from(e + 'I', data, p)[0]
                v = (v & mask) >> shift
                if mask == 0xFFFFFFFF and v >= 0x80000000:
                    v -= 1 << 32
            elif typ == 2:
                v = struct.unpack_from(e + 'f', data, p)[0]
            elif typ == 4:
                v = struct.unpack_from(e + 'h', data, p)[0]
            elif typ == 5:
                v = (data[p] & mask) >> shift
            elif typ == 1:
                v = bytes(data[p:p + 32]).split(b'\0')[0].decode('shift_jis', errors='replace')
            elif typ == 6:
                so = sbase + struct.unpack_from(e + 'I', data, p)[0]
                v = bytes(data[so:data.index(b'\0', so)]).decode('shift_jis', errors='replace')
            else:
                v = None
            row[h] = v
        rows.append(row)
    return fields, rows


def bcsv_hash(name):
    """JMap field-name hash: h = h*31 + c over the bytes, 32-bit."""
    h = 0
    for c in name.encode('shift_jis'):
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


KNOWN_FIELDS = (
    'name', 'l_id', 'CameraSetId', 'SW_APPEAR', 'SW_DEAD', 'SW_A', 'SW_B', 'SW_AWAKE', 'SW_PARAM', 'ParamScale',
    'CastId', 'ViewGroupId', 'ShapeModelNo', 'CommonPath_ID', 'ClippingGroupId', 'GroupId', 'DemoGroupId',
    'MapParts_ID', 'Obj_ID', 'MessageId', 'pos_x', 'pos_y', 'pos_z', 'dir_x', 'dir_y', 'dir_z',
    'scale_x', 'scale_y', 'scale_z', 'Obj_arg0', 'Obj_arg1', 'Obj_arg2', 'Obj_arg3', 'Obj_arg4', 'Obj_arg5',
    'Obj_arg6', 'Obj_arg7', 'Priority', 'AreaShapeNo', 'ShapeModelName', 'MarioNo', 'ScenarioNo', 'type',
    'Camera_id', 'CameraId', 'PlacementCount', 'LayerName', 'string', 'ZoneName', 'StageName', 'GalaxyName',
    'PowerStarId', 'AppearPowerStarObj', 'Valid', 'closed', 'num_pnt', 'point_arg0', 'point_arg1', 'point_arg2',
    'point_arg3', 'point_arg4', 'point_arg5', 'point_arg6', 'point_arg7', 'pnt0_x', 'pnt0_y', 'pnt0_z',
    'pnt1_x', 'pnt1_y', 'pnt1_z', 'pnt2_x', 'pnt2_y', 'pnt2_z', 'path_arg0', 'path_arg1', 'path_arg2',
    'path_arg3', 'path_arg4', 'path_arg5', 'path_arg6', 'path_arg7', 'Rail_ID', 'No', 'Name', 'id', 'ChangeSceneName')


def main(argv):
    import sys
    if len(argv) < 2:
        print('usage: rarc.py ARCHIVE [list | cat PATH | bcsv PATH]')
        return 2
    files = open_archive(argv[1])
    cmd = argv[2] if len(argv) > 2 else 'list'
    if cmd == 'list':
        for n, d in files:
            print('%8d %s' % (len(d), n))
    elif cmd == 'cat':
        sys.stdout.buffer.write(dict(files)[argv[3]])
    elif cmd == 'bcsv':
        names = KNOWN_FIELDS
        hn = {bcsv_hash(n): n for n in names}
        fields, rows = bcsv_rows(dict(files)[argv[3]])
        print(' | '.join(hn.get(h, '%08x' % h) for h, *_ in fields))
        for r in rows:
            print(' | '.join(('%.1f' % r[h]) if isinstance(r[h], float) else str(r[h]) for h, *_ in fields))
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv))

"""Tests for tools/rarc.py (RARC, Yaz0 RARC, cooked CRAR, BCSV) with SYNTHETIC archives built here.  No game data is read or stored."""
import importlib.util
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("rarc", ROOT / "tools/rarc.py")
rarc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rarc)


def build(files, e):
    """files: {'dir/name': bytes}. One root dir 'root' with a sub dir 'stage'; e = '>' (RARC) or '<' (CRAR)."""
    strs = bytearray()

    def s(t):
        o = len(strs)
        strs.extend(t.encode() + b'\0')
        return o
    root, sub = s('root'), s('stage')
    dot, dotdot = s('.'), s('..')
    names = sorted(files)
    ents = []  # (flags, name_off, a, b)
    data = bytearray()
    for n in names:
        off = len(data)
        data.extend(files[n])
        data.extend(b'\0' * (-len(data) % 32))
        ents.append((0x11, s(n.split('/')[-1]), off, len(files[n])))
    root_ents = [(0x02, sub, 1, 0x10), (0x02, dot, 0, 0x10), (0x02, dotdot, 0xFFFFFFFF, 0x10)]
    sub_ents = [(0x02, dot, 1, 0x10), (0x02, dotdot, 0, 0x10)]
    # root dir lists its own entries; keep it simple: root -> [stage folder entry], sub 'stage' -> the files
    allents = root_ents + ents + sub_ents
    dirs = [(root, len(root_ents), 0), (sub, len(ents) + len(sub_ents), len(root_ents))]
    hdr = 0x20
    info = 6 * 4 + 8
    dir_off = info
    file_off = dir_off + 0x10 * len(dirs)
    str_off = file_off + 0x14 * len(allents)
    str_off += -str_off % 32
    data_off = str_off + len(strs)
    data_off += -data_off % 32
    out = bytearray()
    out += b'RARC' if e == '>' else b'CRAR'
    out += struct.pack(e + 'III', 0, hdr, data_off)
    out += struct.pack(e + 'IIII', len(data), len(data), 0, 0)
    out += struct.pack(e + 'IIIIII', len(dirs), dir_off, len(allents), file_off, len(strs), str_off)
    out += b'\0' * (hdr + info - len(out))
    for nm, cnt, first in dirs:
        out += b'DIR ' + struct.pack(e + 'IHHI', nm, 0, cnt, first)
    for flags, nm, a, b in allents:
        if e == '>':
            out += struct.pack('>HHHHIII', 0, 0, flags << 8, nm, a, b, 0)
        else:
            out += struct.pack('<HHIIII', 0, 0, (flags << 24) | nm, a, b, 0)
    out += b'\0' * (hdr + str_off - len(out))
    out += strs
    out += b'\0' * (hdr + data_off - len(out))
    out += data
    return bytes(out)


def yaz0_store(raw):
    """Yaz0 with every byte a literal (valid stream, no matches)."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(raw)) + b'\0' * 8)
    for i in range(0, len(raw), 8):
        chunk = raw[i:i + 8]
        out.append(((0xFF << (8 - len(chunk))) & 0xFF))
        out.extend(chunk)
    return bytes(out)


def bcsv(rows, e):
    """One u32 field 'l_id', one float 'pos_x', one string-pointer 'name'."""
    items = [(rarc.bcsv_hash('l_id'), 0xFFFFFFFF, 0, 0, 0), (rarc.bcsv_hash('pos_x'), 0xFFFFFFFF, 4, 0, 2), (rarc.bcsv_hash('name'), 0xFFFFFFFF, 8, 0, 6)]
    off = 16 + 12 * len(items)
    strs = bytearray()
    body = bytearray()
    for i, x, nm in rows:
        so = len(strs)
        strs += nm.encode() + b'\0'
        body += struct.pack(e + 'IfI', i, x, so)
    out = struct.pack(e + 'iiiI', len(rows), len(items), off, 12)
    for h, m, o, sh, t in items:
        out += struct.pack(e + 'IIHBB', h, m, o, sh, t)
    return out + body + strs


FILES = {'a/one.bin': b'hello', 'a/two.bcsv': b'x' * 100}


class RarcTest(unittest.TestCase):
    def check(self, blob):
        got = dict(rarc.rarc_list(blob))
        self.assertEqual(got, {'root/stage/one.bin': b'hello', 'root/stage/two.bcsv': b'x' * 100})

    def test_rarc(self):
        self.check(build(FILES, '>'))

    def test_crar(self):
        blob = build(FILES, '<')
        self.assertEqual(blob[:4], b'CRAR')
        self.check(blob)

    def test_yaz0_rarc(self):
        self.check(yaz0_store(build(FILES, '>')))

    def test_bad(self):
        with self.assertRaises(ValueError):
            list(rarc.rarc_list(b'NOPE' + b'\0' * 64))

    def test_bcsv_both_endians(self):
        rows = [(7, 1.5, 'Water'), (9, -2.0, 'Cave')]
        for e in '<>':
            f, r = rarc.bcsv_rows(bcsv(rows, e))
            self.assertEqual([x[rarc.bcsv_hash('l_id')] for x in r], [7, 9])
            self.assertEqual([x[rarc.bcsv_hash('name')] for x in r], ['Water', 'Cave'])
            self.assertAlmostEqual(r[1][rarc.bcsv_hash('pos_x')], -2.0)


if __name__ == '__main__':
    unittest.main()

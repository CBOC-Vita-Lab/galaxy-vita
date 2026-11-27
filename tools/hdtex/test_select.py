#!/usr/bin/env python3
"""Unit tests for select_from_census.py and build_pack.py's --list-all / --select-keys, on synthetic cooked archives and census files.
    python3 -E -P tools/hdtex/test_select.py          (needs what build_pack.py imports: numpy, xxhash, Pillow; no game data)
"""
import io
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_pack  # noqa: E402
import select_from_census as sel  # noqa: E402


def bti(fmt, w, h, data):
    hdr = bytearray(0x20)
    hdr[0] = fmt
    struct.pack_into('<HH', hdr, 2, w, h)
    hdr[0x18] = 1
    struct.pack_into('<I', hdr, 0x1C, 0x20)
    struct.pack_into('<I', hdr, 0xC, 0x20)
    return bytes(hdr) + data


def rarc(files):
    """A cooked 'CRAR' with one folder 'root' of [(name, bytes)] (the layout gxarc.rarc_files reads)."""
    strs = bytearray()

    def string(x):
        o = len(strs)
        strs.extend(x.encode() + b'\0')
        return o
    root = string('root')
    names = [string(n) for n, _ in files]
    blob, offs = bytearray(), []
    for _, d in files:
        offs.append(len(blob))
        blob.extend(d + b'\0' * (-len(d) % 32))
    dir_off, file_off = 0x20, 0x30
    str_off = file_off + 0x14 * len(files)
    tab = bytearray(str_off)                       # the table, relative to the header at 0x20 of the file
    struct.pack_into('<6I', tab, 0, 1, dir_off, len(files), file_off, 0, str_off)
    struct.pack_into('<I', tab, dir_off + 4, root)
    struct.pack_into('<H', tab, dir_off + 0xA, len(files))
    struct.pack_into('<I', tab, dir_off + 0xC, 0)
    for i, (n, d) in enumerate(files):
        o = file_off + 0x14 * i
        struct.pack_into('<I', tab, o + 4, (0x11 << 24) | names[i])
        struct.pack_into('<2I', tab, o + 8, offs[i], len(d))
    tab += strs
    tab += b'\0' * (-len(tab) % 32)
    head = bytearray(0x20)
    head[0:4] = b'CRAR'
    struct.pack_into('<I', head, 8, 0x20)
    struct.pack_into('<I', head, 0xC, len(tab))
    return bytes(head) + bytes(tab) + bytes(blob)


def tex(fmt, w, h, seed, alpha=False):
    r = random.Random(seed)
    n = w * h * 2
    d = bytearray(r.randrange(256) for _ in range(n))
    for i in range(0, n, 2):
        if alpha:
            d[i] &= 0x7F  # RGB5A3, top bit 0: 3-bit alpha
        else:
            d[i] |= 0x80  # RGB5A3 top bit 1: opaque
    return bti(fmt, w, h, bytes(d))


def census(path, stage, rows, pics=10, textured=300000):
    with open(path, 'w') as f:
        f.write('# texmag 2\nstage=%s\npictures_sampled=%d\ntextured_px_per_picture=%d\n' % (stage, pics, textured))
        f.write('# cols: key w h fmt player pics px_all px_ge1 px_ge15 px_ge2 px_ge3 px_lod05 px_lod1 px_lod2 mean_mag mean_lod mean_lod_min max_mag min_mag peak_px15 peak_share_pct avg_share_pct\n')
        for k, w, h, player, px_all, px15 in rows:
            f.write('%016x\t%d\t%d\tRGB5A3\t%d\t%d\t%d\t%d\t%d\t0\t0\t0\t0\t0\t1.6\t-0.7\t0\t2\t1\t%d\t5\t3\n' % (k, w, h, player, pics, px_all * pics, px_all * pics, px15 * pics, px15))


class T(unittest.TestCase):
    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix='hdsel')
        for sub in ('ObjectData', 'StageData'):
            os.makedirs(os.path.join(self.d, sub))
        self.tex = {
            'a_body': tex(5, 64, 64, 1), 'a_face': tex(5, 64, 64, 2, alpha=True), 'a_env_map': tex(5, 64, 64, 3), 'a_tiny': tex(5, 8, 8, 4),
            'b_wall': tex(5, 128, 128, 5), 'b_floor': tex(5, 128, 64, 6), 'c_odd': tex(5, 96, 64, 7),
        }
        a = [(n + '.bti', self.tex[n]) for n in ('a_body', 'a_face', 'a_env_map', 'a_tiny')]
        b = [('b_wall.bti', self.tex['b_wall']), ('b_floor.bti', self.tex['b_floor']), ('b_body_again.bti', self.tex['a_body']), ('c_odd.bti', self.tex['c_odd'])]
        open(os.path.join(self.d, 'ObjectData', 'A.arc'), 'wb').write(rarc(a))
        open(os.path.join(self.d, 'ObjectData', 'B.arc'), 'wb').write(rarc(b))
        open(os.path.join(self.d, 'StageData', 'S.arc'), 'wb').write(rarc([('s_sky.bti', tex(5, 64, 32, 9))]))
        self.texs = build_pack.collect(self.d, build_pack.all_archives(self.d))

    def key(self, name):
        for k, e in self.texs.items():
            if any(n.endswith(':' + name + '.bti') or n.endswith(':' + name) for n in e['names']):
                return k
        raise KeyError(name)

    def test_collect_rules(self):
        names = ' '.join(n for e in self.texs.values() for n in e['names'])
        self.assertNotIn('a_env_map', names)  # name rule
        self.assertNotIn('a_tiny', names)     # smaller than 16x16
        self.assertNotIn('c_odd', names)      # 96 is not a power of two
        self.assertIn('StageData/S:s_sky.bti', names)
        self.assertEqual(len(self.texs), 5)    # body (one key, two names), face, wall, floor, sky
        kb = self.key('a_body')
        self.assertEqual(len(self.texs[kb]['names']), 2, 'identical content in two archives is one texture with two names')

    def test_list_all_and_bytes(self):
        out = subprocess.run([sys.executable, '-E', os.path.join(HERE, 'build_pack.py'), '--data', self.d, '--list-all'], capture_output=True, text=True, check=True).stdout
        p = os.path.join(self.d, 'list.tsv')
        open(p, 'w').write(out)
        lst = sel.read_list(p)
        self.assertEqual(set(lst), set(self.texs))
        kb, kf = self.key('a_body'), self.key('a_face')
        self.assertEqual(lst[kb]['alpha'], 0)
        self.assertEqual(lst[kf]['alpha'], 1)
        self.assertEqual(lst[kb]['bytes'], build_pack.pack_bytes(128, 128, 0))
        self.assertEqual(lst[kb]['bytes'], 8 * (1024 + 256 + 64 + 16 + 4 + 1 + 1 + 1))  # 128x128 BC1 down to 1x1 (4x4 blocks, min one)
        self.assertEqual(lst[kf]['bytes'], 2 * lst[kb]['bytes'])

    def run_sel(self, stages, *args):
        files = []
        for i, (name, rows) in enumerate(stages):
            p = os.path.join(self.d, 'texmag-%s.txt' % name)
            census(p, name, rows)
            files.append(p)
        list_p = os.path.join(self.d, 'list.tsv')
        out = subprocess.run([sys.executable, '-E', os.path.join(HERE, 'build_pack.py'), '--data', self.d, '--list-all'], capture_output=True, text=True, check=True).stdout
        open(list_p, 'w').write(out)
        o = os.path.join(self.d, 'sel.txt')
        r = io.StringIO()
        old, sys.stdout = sys.stdout, r
        try:
            sel.main(files + ['--list', list_p, '--out', o] + list(args))
        finally:
            sys.stdout = old
        keys, _ = build_pack.read_keylist(o)
        return keys, r.getvalue(), o

    def test_rule_and_budgets(self):
        kb, kf, kw, kfl, ks = (self.key(n) for n in ('a_body', 'a_face', 'b_wall', 'b_floor', 's_sky'))
        stages = [('0000aaaa', [(kb, 64, 64, 0, 40000, 30000), (kf, 64, 64, 0, 9000, 1500), (kw, 128, 128, 0, 80000, 50000), (kfl, 128, 64, 0, 20000, 2500),
                                (0xdeadbeef, 64, 64, 0, 90000, 90000)]),
                  ('0000bbbb', [(ks, 64, 32, 0, 5000, 4000), (kw, 128, 128, 0, 10000, 100)])]
        keys, text, _ = self.run_sel(stages, '--min-area', '2000', '--total-mb', '100', '--stage-mb', '100')
        self.assertEqual(keys, {kb, kw, kfl, ks})  # face: only 1500 magnified px (< A); the unknown key is never selected
        self.assertIn('not in the list', text)
        # the 4x rule: the file has no scale at all, and the builder's --select-keys builds at --scale (2 by default)
        # a stage budget that holds one of the two big ones (BC1 128x128 = 10.9 KiB, 128x64 = 5.5 KiB): the best per byte first
        keys, text, _ = self.run_sel(stages, '--min-area', '2000', '--total-mb', '100', '--stage-mb', '0.012')
        self.assertTrue(keys <= {kb, kw, kfl, ks} and len(keys) >= 1)
        per = {}
        for k in keys:
            self.assertLessEqual(build_pack.pack_bytes(2 * self.texs[k]['t'].w, 2 * self.texs[k]['t'].h, 0), 0.012 * 1048576)
        self.assertIn('stage budget', text)
        keys, text, _ = self.run_sel(stages, '--min-area', '2000', '--total-mb', '0.001', '--stage-mb', '100')
        self.assertEqual(keys, set())
        self.assertIn('total budget', text)

    def test_player(self):
        kf = self.key('a_face')
        stages = [('0000aaaa', [(kf, 64, 64, 1, 9000, 0)])]
        keys, _, _ = self.run_sel(stages, '--min-area', '2000')
        self.assertEqual(keys, set())
        keys, text, _ = self.run_sel(stages, '--min-area', '2000', '--include-player')
        self.assertEqual(keys, {kf})
        self.assertIn('player', text)

    def test_select_keys_builds_those(self):
        kb, kw = self.key('a_body'), self.key('b_wall')
        p = os.path.join(self.d, 'k.txt')
        open(p, 'w').write('# x\n%016x  # a comment\nB:b_floor.bti\n' % kb)
        got = build_pack.collect(self.d, build_pack.all_archives(self.d), build_pack.read_keylist(p))
        self.assertEqual(set(got), {kb, self.key('b_floor')})
        self.assertNotIn(kw, got)


if __name__ == '__main__':
    unittest.main()

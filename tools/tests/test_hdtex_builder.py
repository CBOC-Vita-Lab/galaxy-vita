"""HD pack builder: quality floor with its fallback, mip chain, pack order, the report; all on synthetic textures (no game data).
    python3 -E -P tools/tests/test_hdtex_builder.py      (needs numpy, Pillow, xxhash, quicktex)"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'hdtex'))
import hdtex_model as hm  # noqa: E402

try:
    import build_pack as bp  # noqa: E402
    import quicktex  # noqa: E402,F401
    HAVE = True
except ImportError:  # xxhash / quicktex / Pillow missing
    HAVE = False


def smooth(n=64):
    y, x = np.mgrid[0:n, 0:n] / n
    rgb = np.stack([0.78 + 0.15 * x, 0.58 + 0.2 * y, 0.45 + 0.1 * x * y], -1)
    return np.concatenate([rgb, np.ones((n, n, 1))], -1).astype(np.float32)


def noise(n=64, seed=1, sigma=0.17):
    rng = np.random.default_rng(seed)
    rgb = np.clip(0.5 + rng.normal(0, sigma, (n, n, 3)), 0, 1)
    return np.concatenate([rgb, np.ones((n, n, 1))], -1).astype(np.float32)


class T:  # what write_pack reads of a texture
    def __init__(self, fmt=0xE, w=8, h=8):
        self.fmt, self.w, self.h = fmt, w, h


class Mips(unittest.TestCase):
    def test_box_average_exactly(self):
        a = np.random.default_rng(3).random((16, 8, 4)).astype(np.float32)
        a[..., 3] = 1
        lv = hm.make_mips(a)
        self.assertEqual([x.shape[:2] for x in lv], [(16, 8), (8, 4), (4, 2), (2, 1), (1, 1)])
        want = a[..., :3].reshape(8, 2, 4, 2, 3).mean((1, 3))
        self.assertTrue(np.allclose(lv[1][..., :3], want, atol=1e-6))
        self.assertTrue(np.allclose(lv[-1][0, 0, :3], a[..., :3].mean((0, 1)), atol=1e-5))

    def test_colour_weighted_by_alpha(self):
        a = np.zeros((4, 4, 4), np.float32)
        a[:, :2] = [1, 0, 0, 1]  # opaque red on the left, transparent black (colour never filled in) on the right
        lv = hm.make_mips(a)
        self.assertTrue(np.allclose(lv[1][:, 0, :3], [1, 0, 0]), 'a half-covered texel keeps the colour of what covers it')
        plain = hm.make_mips(a, alpha_weighted=False)
        self.assertTrue(np.allclose(plain[2][0, 0, :3], [0.5, 0, 0]), 'without the weighting it would darken')
        self.assertTrue(np.allclose([x[..., 3].mean() for x in lv], 0.5), 'coverage (mean alpha) is kept at every level')

    def test_linear_light_option(self):
        a = np.zeros((2, 2, 4), np.float32)
        a[..., 3] = 1
        a[0] = 1  # one white row, one black
        self.assertAlmostEqual(float(hm.make_mips(a)[1][0, 0, 0]), 0.5, places=6)
        self.assertAlmostEqual(float(hm.make_mips(a, gamma=2.2)[1][0, 0, 0]), 0.5 ** (1 / 2.2), places=5)


@unittest.skipUnless(HAVE, 'needs xxhash, Pillow and quicktex')
class Dxt(unittest.TestCase):
    def test_decoder_matches_quicktex(self):
        import quicktex.s3tc.bc1 as bc1
        import quicktex.s3tc.bc3 as bc3
        rng = np.random.default_rng(5)
        img = np.clip(rng.normal(120, 70, (32, 48, 4)), 0, 255).astype(np.uint8)
        for fmt, mod, name in ((1, bc1, 'BC1'), (3, bc3, 'BC3')):
            if fmt == 1:
                img[..., 3] = 255
            blob, dec = bp.encode(img, 48, 32, fmt)
            mine = hm.decode_dxt(blob, 48, 32, fmt)
            self.assertLessEqual(int(np.abs(mine.astype(int) - dec.astype(int)).max()), 2, name)
            self.assertLess(float(np.abs(mine.astype(int) - dec.astype(int)).mean()), 0.3, name)

    def test_psnr_is_colour_only_and_block_worst(self):
        ref = (smooth(16) * 255).astype(np.uint8)
        dec = ref.copy()
        dec[..., :3] = np.clip(ref[..., :3].astype(int) + 4, 0, 255)
        self.assertAlmostEqual(hm.psnr_rgb(ref, dec), 10 * np.log10(255.0 ** 2 / 16), places=3)  # constant alpha does not count
        dec2 = ref.copy()
        dec2[:4, :4, :3] = 0
        self.assertLess(hm.worst_block_psnr(ref, dec2), 8.0)
        self.assertEqual(hm.psnr_rgb(ref, ref), 99.0)

    def test_floor_keeps_smooth_art_and_blends_noise_back(self):
        soft = smooth(64)
        r = bp.finish(soft, soft, True, False, floor=34.0)
        self.assertIsNone(r['rejected'])
        self.assertEqual((r['blend'], len(r['levels'])), (1.0, 7))
        self.assertGreater(r['psnr'], 40)
        # a noisy 'network output' over a smooth plain enlargement: level 0 misses the floor at full share, passes at a lower one
        hd = np.clip(0.6 * soft + 0.4 * noise(64, 2, 0.12), 0, 1)
        hd[..., 3] = 1
        r1 = bp.finish(hd, soft, True, False, floor=0.0)
        self.assertEqual(r1['blend'], 1.0)
        floor = r1['psnr'] + 2.0
        r2 = bp.finish(hd, soft, True, False, floor=floor)
        self.assertLess(r2['blend'], 1.0, 'the network share was lowered')
        self.assertIsNone(r2['rejected'])
        self.assertGreaterEqual(r2['psnr'], floor)

    def test_texture_that_misses_the_floor_everywhere_is_left_out(self):
        n = noise(64, 7, 0.3)
        r = bp.finish(n, noise(64, 8, 0.3), True, False, floor=34.0)
        self.assertIn('floor', r['rejected'])
        self.assertEqual(r['levels'], [])
        self.assertIsNone(bp.finish(n, noise(64, 8, 0.3), True, False, floor=0.0)['rejected'], 'floor 0 = off')

    def test_alpha_texture_is_dxt5_and_keeps_its_on_off_alpha(self):
        a = smooth(32)
        a[..., 3] = (np.mgrid[0:32, 0:32][1] < 16).astype(np.float32) * 0.9 + 0.05  # a soft network alpha around 0.05 / 0.95
        r = bp.finish(a, a, False, True, floor=30.0)
        self.assertEqual(r['fmt'], bp.PACK_DXT5)
        dec0 = r['hd'][..., 3]
        self.assertTrue(np.isin(dec0, (0, 255)).all(), 'level 0 alpha is on/off again after thresholding')
        self.assertEqual(len(r['levels']), 6)


@unittest.skipUnless(HAVE, 'needs xxhash, Pillow and quicktex')
class PackFile(unittest.TestCase):
    def make(self, specs):
        """specs: [(names, prio, hd side)] -> pack path; one fake original (8x8 CMPR) per spec, keys from the spec's index."""
        items = []
        for i, (names, prio, side) in enumerate(specs):
            lv, s = [], side
            while True:
                lv.append(bytes([i + 1]) * (max(1, (s + 3) // 4) ** 2 * 8))
                if s == 1:
                    break
                s //= 2
            hd = dict(fmt=bp.PACK_DXT1, w=side, h=side, levels=lv, opaque=True)
            items.append((0x1000 + 7 * i if i % 2 else 0x9000 - i, dict(names=names, t=T(), prio=prio), hd))
        d = tempfile.mkdtemp()
        p = os.path.join(d, 'x.pack')
        bp.write_pack(p, items)
        return p

    def test_key_function_is_unchanged(self):
        # (the engine's HdKey() and every pack ever built depend on it)
        level0 = bytes((i * 7 + 3) & 255 for i in range(32))
        self.assertEqual(bp.tex_key(0xE, 8, 8, level0, b''), 0x79de773626b3151e)

    def test_data_in_selection_order_index_by_key(self):
        p = self.make([(['Late:A'], 2, 16), (['First:B'], 0, 16), (['Mid:C'], 1, 16), (['First:D'], 0, 16)])
        hdr, ents, problems = hm.read_pack(p)
        self.assertEqual(problems, [])
        self.assertEqual([e['key'] for e in ents], sorted(e['key'] for e in ents))
        by_off = [e['names'][0] for e in sorted(ents, key=lambda e: e['off'])]
        self.assertEqual(by_off, ['First:B', 'First:D', 'Mid:C', 'Late:A'])
        self.assertTrue(all(e['off'] % 64 == 0 for e in ents))

    def test_reader_notes_damage(self):
        p = self.make([(['A:a'], 0, 16)])
        with open(p, 'rb') as f:
            b = f.read()
        with open(p, 'wb') as f:
            f.write(b[:-40])
        self.assertTrue(any('copied in part' in x or 'past the end' in x for x in hm.read_pack(p)[2]))

    def test_screen_notes(self):
        table = hm.read_screensize()
        self.assertTrue(table)
        v = lambda names, w, h: hm.screen_note(names, w, h, table)[3]
        self.assertEqual(v(['Mario:MarioBody.0'], 256, 128), 'closeups')
        self.assertEqual(v(['Koopa:KoopaBody'], 256, 512), 'gameplay-partly')
        self.assertEqual(v(['Mario:MarioFace'], 128, 128), 'closeups')
        self.assertEqual(v(['Nobody:Nothing'], 64, 64), 'unknown')
        # a tiny texture on a big object is resolved at the usual size: HD pays there
        self.assertEqual(hm.screen_note(['X:y'], 16, 16, [(__import__('re').compile('X'), 100, 200)])[3], 'gameplay')

    def test_scene_estimates_and_report(self):
        names = [['Mario:MarioBody.0'], ['Mario:MarioBody.1'], ['Mario:MarioFace', 'BeeMario:MarioFace'], ['Koopa:KoopaBody'], ['Rosetta:Rosetta_all']]
        p = self.make([(n, i, 32) for i, n in enumerate(names)])
        out = []
        res = hm.report(p, out=out.append, quality=True)
        self.assertEqual(res['problems'], [])
        self.assertEqual(res['records'], 5)
        n_mario = res['scenes']['typical stage: Mario'][0]
        self.assertEqual(n_mario, 2, 'MarioBody.1 (another form) is not in the usual stage; the face is')
        self.assertEqual(res['scenes']['Bowser fight: Mario, Bowser'][0], 3)
        self.assertEqual(res['scenes']['every record of the pack'][0], 5)
        self.assertEqual(res['console_bytes'], 5 * (512 + 128 + 32 + 8))  # the console takes 32, 16, 8 and 4 texels wide (DXT1: 64, 16, 4 and 1 blocks)
        self.assertTrue(any('every record' in l for l in out))
        self.assertEqual(res['warnings'], [])


if __name__ == '__main__':
    unittest.main()

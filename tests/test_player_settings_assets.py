import importlib.util
import pathlib
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PlayerSettingsAssets(unittest.TestCase):
    def test_livearea_keeps_start_and_background_and_has_no_settings_tile(self):
        tree = ET.parse(ROOT / 'vita/vgx-game/sce_sys/livearea/contents/template.xml')
        root = tree.getroot()
        self.assertEqual(root.findtext('gate/startup-image'), 'startup.png')
        self.assertEqual(root.findtext('livearea-background/image'), 'bg.png')
        # The Settings tile is gone: Settings live in the game (Single Player / Multiplayer screen and the pause menu).
        self.assertEqual([f for f in root.iter('frame')], [])
        self.assertEqual([t.text for t in root.iter('target')], [])
        self.assertNotIn('settings.png', [i.text for i in root.iter('image')])

    def test_owned_bitmap_generation_is_reproducible(self):
        spec = importlib.util.spec_from_file_location('settings_assets', ROOT / 'tools/handheld/make_settings_assets.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            module.ROOT = pathlib.Path(temporary)
            (module.ROOT / 'platform/src/settings').mkdir(parents=True)
            (module.ROOT / 'vita/vgx-game/sce_sys/livearea/contents').mkdir(parents=True)
            module.main()
            for relative in ['platform/src/settings/settings_assets.inc', 'platform/src/settings/settings_vita.inc', 'platform/src/settings/settings_playstation.inc']:
                self.assertEqual((module.ROOT / relative).read_bytes(), (ROOT / relative).read_bytes())
        # (fix31) the generator writes no LiveArea picture; the settings.png beside the template is the contributor's (ARTWORK.md), kept as supplied, and is
        # no longer named by the template nor put in the VPK (the Settings tile is gone)
        png = (ROOT / 'vita/vgx-game/sce_sys/livearea/contents/settings.png').read_bytes()
        self.assertFalse((module.ROOT / 'vita/vgx-game/sce_sys/livearea/contents/settings.png').exists())
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>IIBBBBB', png[16:29]), (280, 158, 8, 3, 0, 0, 0))
        pixels = module.rgba_png(ROOT / 'tools/handheld/assets/settings_vita.png')
        self.assertEqual(len(pixels), 300*129*4)
        self.assertGreater(len(set(zip(pixels[0::4], pixels[1::4], pixels[2::4], pixels[3::4]))), 256)
        self.assertIn(0, pixels[3::4])
        self.assertTrue(any(0 < alpha < 255 for alpha in pixels[3::4]))
        controller = module.rgba_png(ROOT / 'tools/handheld/assets/settings_playstation.png', (250, 167))
        self.assertEqual(len(controller), 250*167*4)
        self.assertGreater(len(set(zip(controller[0::4], controller[1::4], controller[2::4], controller[3::4]))), 256)
        self.assertIn(0, controller[3::4])



class PackagePictures(unittest.TestCase):
    """(fix31) The bubble and LiveArea pictures are the five contributor files of vita/vgx-game/sce_sys (ARTWORK.md, the repo's AGENTS.md): approved by
    the owner, shipped as they are, never regenerated or replaced. They include the game's key art and logo (docs/ASSETS.md). Before fix31 these tests
    expected the neutral drawings of tools/handheld/make_livearea.py and were red on the approved art."""
    SHIPPED = {'icon0.png': ('vita/vgx-game/sce_sys/icon0.png', (128, 128)), 'pic0.png': ('vita/vgx-game/sce_sys/pic0.png', (960, 544)),
               'livearea/contents/bg.png': ('vita/vgx-game/sce_sys/livearea/contents/bg.png', (840, 500)),
               'livearea/contents/startup.png': ('vita/vgx-game/sce_sys/livearea/contents/startup.png', (280, 158)),
               'livearea/contents/settings.png': ('vita/vgx-game/sce_sys/livearea/contents/settings.png', (280, 158))}
    # sha256 of the approved 3.0 bubble & LiveArea files: any accidental regeneration fails here
    APPROVED = {'icon0.png': '7fa63a19c822bff68d23aacada5b7e1fc97d698364a1145b4159c72ec82a7b2a',
                'pic0.png': '59668c9c65aa3b6550bb4ecee07ab3158f74bf8e28070c9a14da07569b681096',
                'livearea/contents/bg.png': '3d57068135279416adc97b7fe99e2b96fa7d26fdbeb2d493ba63210c0343fa3a',
                'livearea/contents/startup.png': 'd1f6e806f1f11f0613abd3ae65dad066eccbde148a8bf47e3ea6f0c91f1bd2c2',
                'livearea/contents/settings.png': '3ed8b34c9e5dcee4df2dcd75728641c577b12fa2affe0e6ee3eca1c7451796ed'}

    def test_pictures_are_8bit_palette_pngs_of_the_right_size(self):
        for name, (relative, size) in self.SHIPPED.items():
            png = (ROOT / relative).read_bytes()
            with self.subTest(name=name):
                self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(struct.unpack('>II', png[16:24]), size)
                self.assertEqual(png[24:26], b'\x08\x03', '8-bit palette PNG')

    def test_pictures_are_the_approved_contributor_files(self):
        import hashlib
        for name, (relative, _) in self.SHIPPED.items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest(), self.APPROVED[name])
        self.assertIn('Contributor artwork', (ROOT / 'vita/vgx-game/sce_sys/ARTWORK.md').read_text())

    def test_pictures_pass_the_format_checker(self):
        import subprocess, sys
        result = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/handheld/check_livearea.py'), str(ROOT / 'vita/vgx-game/sce_sys')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_format_checker_allows_alpha_only_in_the_gate_image(self):
        # (fix31) the gate's startup.png may carry palette alpha (tRNS, as the approved one does); the bubble icon may not
        import shutil, subprocess, sys, zlib
        with tempfile.TemporaryDirectory() as temporary:
            folder = pathlib.Path(temporary) / 'sce_sys'
            shutil.copytree(ROOT / 'vita/vgx-game/sce_sys', folder)
            icon = (folder / 'icon0.png').read_bytes()
            at = icon.index(b'IDAT') - 4
            body = bytes([255] * 4)
            trns = struct.pack('>I', len(body)) + b'tRNS' + body + struct.pack('>I', zlib.crc32(b'tRNS' + body) & 0xffffffff)
            (folder / 'icon0.png').write_bytes(icon[:at] + trns + icon[at:])
            result = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/handheld/check_livearea.py'), str(folder)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("extra chunks [b'tRNS']", result.stdout)
            self.assertIn('ok   livearea/contents/startup.png', result.stdout)

    def test_neutral_generator_draws_no_game_artwork(self):
        # (the generator stays as a neutral fallback for builds without the contributor art; its own pictures are drawn in code)
        source = (ROOT / 'tools/handheld/make_livearea.py').read_text()
        self.assertNotIn('title.rgba', source)  # drawn in code, never from a capture
        self.assertNotIn('Image.open', source)  # nothing read from a picture file
        self.assertIn('Inter-OFL.txt', source)
        self.assertTrue((ROOT / 'licenses/Inter-OFL.txt').is_file())

    def test_reference_script_still_documents_the_device_algorithm(self):
        text = (ROOT / 'tools/handheld/make_livearea_title.py').read_text()
        self.assertIn('title_art.h', text)
        self.assertIn('reference', text.lower())


if __name__ == '__main__':
    unittest.main()

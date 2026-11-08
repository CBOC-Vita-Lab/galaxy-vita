import importlib.util
import pathlib
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PlayerSettingsAssets(unittest.TestCase):
    def test_livearea_keeps_start_and_background(self):
        tree = ET.parse(ROOT / 'vita/vgx-game/sce_sys/livearea/contents/template.xml')
        root = tree.getroot()
        self.assertEqual(root.findtext('gate/startup-image'), 'startup.png')
        self.assertEqual(root.findtext('livearea-background/image'), 'bg.png')
        settings = [frame for frame in root.findall('frame') if frame.findtext('liveitem/target') == 'psla:-settings']
        self.assertEqual(len(settings), 1)
        self.assertEqual(settings[0].findtext('liveitem/image'), 'settings.png')

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
        png = (ROOT / 'vita/vgx-game/sce_sys/livearea/contents/settings.png').read_bytes()
        self.assertFalse((module.ROOT / 'vita/vgx-game/sce_sys/livearea/contents/settings.png').exists())
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>IIBBBBB', png[16:29]), (280, 124, 8, 3, 0, 0, 0))
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
    """The public package's bubble and LiveArea pictures are the neutral drawings of make_livearea.py (no game artwork); the
    game replaces them on the console with ones made from the player's own title screen (tests/test_title_art.py)."""
    SHIPPED = {'icon0.png': ('vita/vgx-game/sce_sys/icon0.png', (128, 128)), 'pic0.png': ('vita/vgx-game/sce_sys/pic0.png', (960, 544)),
               'livearea/contents/bg.png': ('vita/vgx-game/sce_sys/livearea/contents/bg.png', (840, 500)),
               'livearea/contents/startup.png': ('vita/vgx-game/sce_sys/livearea/contents/startup.png', (280, 158)),
               'livearea/contents/settings.png': ('vita/vgx-game/sce_sys/livearea/contents/settings.png', (280, 124))}

    def test_pictures_are_8bit_palette_pngs_of_the_right_size(self):
        for name, (relative, size) in self.SHIPPED.items():
            png = (ROOT / relative).read_bytes()
            with self.subTest(name=name):
                self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(struct.unpack('>II', png[16:24]), size)
                self.assertEqual(png[24:26], b'\x08\x03', '8-bit palette PNG')

    def test_neutral_pictures_are_reproducible_and_carry_no_version(self):
        try:
            from PIL import ImageFont
            ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 10)
        except Exception as error:
            self.skipTest(f'generator dependencies missing: {error}')
        import subprocess, sys
        source = (ROOT / 'tools/handheld/make_livearea.py').read_text()
        self.assertNotIn('version', source.lower().replace('versions', ''))
        with tempfile.TemporaryDirectory() as temporary:
            subprocess.run([sys.executable, '-I', str(ROOT / 'tools/handheld/make_livearea.py'), temporary], check=True, capture_output=True)
            for name, (relative, _) in self.SHIPPED.items():
                from PIL import Image  # (decoded pixels and palette: PNG compression differs between zlib builds)
                import numpy as np  # (floating-point noise and palette rounding differ slightly between numpy/Pillow builds: near-identical, not byte-identical)
                made = np.asarray(Image.open(pathlib.Path(temporary) / name).convert('RGB'), np.int16)
                shipped = np.asarray(Image.open(ROOT / relative).convert('RGB'), np.int16)
                self.assertEqual(made.shape, shipped.shape, name)
                diff = np.abs(made - shipped)
                self.assertLess(diff.mean(), 2.0, name)
                self.assertGreater((diff.max(axis=2) <= 24).mean(), 0.98, name)

    def test_reference_script_still_documents_the_device_algorithm(self):
        text = (ROOT / 'tools/handheld/make_livearea_title.py').read_text()
        self.assertIn('title_art.h', text)
        self.assertIn('reference', text.lower())


if __name__ == '__main__':
    unittest.main()

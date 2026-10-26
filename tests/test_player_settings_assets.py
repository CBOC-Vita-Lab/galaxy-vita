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
            for relative in ['platform/src/settings/settings_assets.inc', 'platform/src/settings/settings_vita.inc', 'platform/src/settings/settings_playstation.inc', 'vita/vgx-game/sce_sys/livearea/contents/settings.png']:
                self.assertEqual((module.ROOT / relative).read_bytes(), (ROOT / relative).read_bytes())
        png = (ROOT / 'vita/vgx-game/sce_sys/livearea/contents/settings.png').read_bytes()
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>IIBBBBB', png[16:29]), (280, 64, 8, 3, 0, 0, 0))
        pixels = module.rgba_png(ROOT / 'tools/handheld/assets/settings_vita.png')
        self.assertEqual(len(pixels), 300*129*4)
        self.assertGreater(len(set(zip(pixels[0::4], pixels[1::4], pixels[2::4], pixels[3::4]))), 256)
        self.assertIn(0, pixels[3::4])
        self.assertTrue(any(0 < alpha < 255 for alpha in pixels[3::4]))
        controller = module.rgba_png(ROOT / 'tools/handheld/assets/settings_playstation.png', (250, 167))
        self.assertEqual(len(controller), 250*167*4)
        self.assertGreater(len(set(zip(controller[0::4], controller[1::4], controller[2::4], controller[3::4]))), 256)
        self.assertIn(0, controller[3::4])


if __name__ == '__main__':
    unittest.main()

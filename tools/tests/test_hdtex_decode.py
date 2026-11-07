"""Wii CMPR palette rules used when preparing HD textures."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'hdtex'))
import gxdecode


class CmprDecodeTests(unittest.TestCase):
    def palette(self, first, second, width=8, height=8):
        # Every sub-block row selects entries 0, 1, 2, 3 from left to right.
        block = struct.pack('>HH4B', first, second, 0x1b, 0x1b, 0x1b, 0x1b)
        return gxdecode.decode(14, width, height, block * 4)

    def test_opaque_palette_uses_five_eighths(self):
        rgba = self.palette(0xffff, 0)
        self.assertEqual(rgba[0, :4].tolist(),
                         [[255, 255, 255, 255], [0, 0, 0, 255],
                          [159, 159, 159, 255], [95, 95, 95, 255]])
        self.assertEqual(rgba[4, 4:].tolist(), rgba[0, :4].tolist())

    def test_transparent_entry_preserves_average_rgb(self):
        rgba = self.palette(0, 0xffff)
        self.assertEqual(rgba[0, :4].tolist(),
                         [[0, 0, 0, 255], [255, 255, 255, 255],
                          [127, 127, 127, 255], [127, 127, 127, 0]])

    def test_partial_tiles_keep_palette_and_crop(self):
        rgba = self.palette(0xf800, 0x07e0, 5, 3)
        self.assertEqual(rgba.shape, (3, 5, 4))
        self.assertEqual(rgba[0].tolist(),
                         [[255, 0, 0, 255], [0, 255, 0, 255],
                          [159, 95, 0, 255], [95, 159, 0, 255], [255, 0, 0, 255]])


if __name__ == '__main__':
    unittest.main()

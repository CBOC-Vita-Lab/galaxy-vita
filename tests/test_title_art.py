"""The game builds its loading, Settings and home-screen pictures from the player's own title screen (port/title_art.h).
Host checks of that code: the deflate against zlib, the quantizer and PNG writer against Pillow, the trigger and path logic, the
picture path on a made-up title, and (when the owner's capture is at hand) the real capture against the reference script
tools/handheld/make_livearea_title.py by mean colour error."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib

import numpy
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
# A real title capture (960x544 RGBA) from your own game, for the comparison with the reference script: TITLEART_CAPTURE=<file>, or
# local/title-capture.rgba in the tree (not tracked). Without one that test is skipped.
CAPTURE = pathlib.Path(os.environ.get("TITLEART_CAPTURE", str(ROOT / "local/title-capture.rgba")))
SIZES = {"pic0": (960, 544), "bg": (840, 500), "startup": (280, 158), "icon0": (128, 128)}


def compiler():
    for name in (os.environ.get("CXX"), "clang++", "g++", "c++"):
        if name and shutil.which(name):
            return name
    return None


class TitleArt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cxx = compiler()
        if cxx is None:
            raise unittest.SkipTest("no C++ compiler")
        cls.temp = tempfile.TemporaryDirectory(prefix="titleart-")
        cls.dir = pathlib.Path(cls.temp.name)
        cls.binary = cls.dir / "title_art"
        subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "platform/include"), "-I", str(ROOT / "tests"),
                        str(ROOT / "tests/title_art.cpp"), "-o", str(cls.binary)], check=True, capture_output=True, text=True, timeout=300)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_mode(self, *args):
        result = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)
        return result.stdout

    def test_trigger_decisions_and_paths(self):
        self.run_mode("logic")

    def test_deflate_streams_decode_with_zlib(self):
        out = self.dir / "deflate"
        out.mkdir()
        self.run_mode("deflate", str(out))
        for name in (out / "deflate-cases.txt").read_text().split():
            raw = (out / f"{name}.raw").read_bytes()
            packed = (out / f"{name}.z").read_bytes()
            with self.subTest(name=name):
                self.assertEqual(zlib.decompress(packed), raw)
                decoder = zlib.decompressobj()  # a complete stream with nothing after it
                decoder.decompress(packed)
                self.assertTrue(decoder.eof)
                self.assertEqual(decoder.unused_data, b"")
                if len(raw) > 1000:
                    # never much worse than zlib at its best, and never expands incompressible data by more than the stored-block overhead
                    self.assertLessEqual(len(packed), len(zlib.compress(raw, 9)) * 1.25 + 64)
                    self.assertLessEqual(len(packed), len(raw) + len(raw) // 1000 + 64)
        self.assertLess(len((out / "zeros.z").read_bytes()), 400)

    def test_quantizer_and_png_writer(self):
        out = self.dir / "png"
        out.mkdir()
        self.run_mode("png", str(out))
        bounds = {  # name: (mean abs error, max abs error) per channel
            "gradient": (6.0, 24), "noise": (14.0, 80), "flat": (0, 0), "two": (0, 0), "few": (0, 0), "tiny": (0, 0),
            "thin-tall": (1.5, 3), "thin-wide": (1.5, 3), "stripes": (2.5, 24), "flat-nodither": (3.5, 12),
        }
        seen = 0
        for line in (out / "png-cases.txt").read_text().splitlines():
            name, w, h, _ = line.split()
            w, h = int(w), int(h)
            data = (out / f"{name}.png").read_bytes()
            with self.subTest(name=name):
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                self.assertEqual(data[24:26], b"\x08\x03", "8-bit palette")
                image = Image.open(out / f"{name}.png")
                image.verify()
                image = Image.open(out / f"{name}.png")
                image.load()
                self.assertEqual((image.format, image.mode, image.size), ("PNG", "P", (w, h)))
                self.assertLessEqual(len(image.getpalette()) // 3, 256)
                source = numpy.frombuffer((out / f"{name}.rgb").read_bytes(), numpy.uint8).reshape(h, w, 3).astype(int)
                error = numpy.abs(numpy.asarray(image.convert("RGB")).astype(int) - source)
                mean, worst = bounds[name]
                self.assertLessEqual(error.mean(), mean)
                self.assertLessEqual(error.max(), worst)
            seen += 1
        self.assertEqual(seen, len(bounds))

    def test_made_up_title_through_the_whole_path(self):
        out = self.dir / "synthetic"
        out.mkdir()
        self.run_mode("synthetic", str(out))
        for name, size in SIZES.items():
            data = (out / f"{name}.png").read_bytes()
            with self.subTest(name=name):
                self.assertEqual(data[24:26], b"\x08\x03")
                image = Image.open(out / f"{name}.png")
                image.load()
                self.assertEqual((image.mode, image.size), ("P", size))
        # pic0 is the title without its text: the strokes' exact colours are gone from the text rectangle
        pic = numpy.asarray(Image.open(out / "pic0.png").convert("RGB")).astype(int)
        region = pic[392:518, 222:738]
        self.assertLess((region.sum(axis=2) > 600).mean(), 0.03)

    def test_real_capture_against_the_reference_script(self):
        if not CAPTURE.is_file() or CAPTURE.stat().st_size != 960 * 544 * 4:
            self.skipTest("no title capture at hand (TITLEART_CAPTURE)")
        try:
            import cv2  # noqa: F401
        except ImportError:
            self.skipTest("OpenCV is needed for the reference script")
        mine, reference = self.dir / "mine", self.dir / "reference"
        mine.mkdir()
        reference.mkdir()
        self.run_mode("pictures", str(CAPTURE), str(mine))
        subprocess.run([sys.executable, "-I", str(ROOT / "tools/handheld/make_livearea_title.py"), str(CAPTURE), str(reference)], capture_output=True)
        capture = numpy.asarray(Image.frombytes("RGBA", (960, 544), CAPTURE.read_bytes()).convert("RGB")).astype(int)
        for name, size in SIZES.items():
            with self.subTest(name=name):
                a = Image.open(mine / f"{name}.png")
                b = Image.open(reference / f"{name}.png")
                a.load()
                self.assertEqual((a.mode, a.size), ("P", size))
                self.assertEqual((b.mode, b.size), ("P", size))
                self.assertLessEqual(len(a.getpalette()) // 3, 256)
                error = numpy.abs(numpy.asarray(a.convert("RGB")).astype(int) - numpy.asarray(b.convert("RGB")).astype(int))
                self.assertLess(error.mean(), 4.5, f"{name}: mean colour error against the reference")
        a = numpy.asarray(Image.open(mine / "pic0.png").convert("RGB")).astype(int)
        b = numpy.asarray(Image.open(reference / "pic0.png").convert("RGB")).astype(int)
        # Inside the text rectangle (where the two fills differ most) and outside it (where both only quantise the capture).
        inside = numpy.zeros((544, 960), bool)
        inside[384:526, 214:746] = True
        self.assertLess(numpy.abs(a - b)[inside].mean(), 4.5)
        self.assertLess(numpy.abs(a - capture)[~inside].mean(), 3.5)
        self.assertLess(numpy.abs(b - capture)[~inside].mean(), 3.5)
        # The text is gone from the picture.
        region = a[392:518, 222:738]
        self.assertLess(((region.sum(axis=2) > 330) | (region[..., 2] < region[..., 0] + 30)).mean(), 0.05)


if __name__ == "__main__":
    unittest.main()

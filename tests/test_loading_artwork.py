"""Preserve the selected loading picture and execute its CMake packaging guard."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSET = ROOT / "vita/vgx-game/sce_sys/loading-title.rgba"
MODULE = ROOT / "vita/vgx-game/loading_artwork.cmake"
SIZE = 960 * 544 * 4
DIGEST = "14529d51dc9e3c4831a20f389b90c162f93ddeafc7567b206fe3b976dd679906"


class LoadingArtworkTests(unittest.TestCase):
    def test_preserved_title_pixels(self):
        self.assertTrue(ASSET.is_file(), "The selected loading picture must be in the source tree")
        data = ASSET.read_bytes()
        self.assertEqual(len(data), SIZE)
        self.assertEqual(hashlib.sha256(data).hexdigest(), DIGEST)

    def test_build_uses_checked_artwork(self):
        source = (ROOT / "vita/vgx-game/CMakeLists.txt").read_text()
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/loading_artwork.cmake")', source)
        self.assertIn("${smg_loading_picture}", source)

    def configure(self, data):
        with tempfile.TemporaryDirectory(prefix="galaxy-loading-") as temp:
            root = Path(temp)
            if data is not None:
                (root / "loading-title.rgba").write_bytes(data)
            script = root / "check.cmake"
            record = root / "record.txt"
            script.write_text('include("${MODULE}")\nfile(WRITE "${RECORD}" "${smg_loading_picture}")\n')
            result = subprocess.run([shutil.which("cmake"), f"-DMODULE={MODULE}",
                                     f"-Dsys={root}", f"-DRECORD={record}", "-P", str(script)],
                                    capture_output=True, text=True, timeout=15)
            output = record.read_text() if record.exists() else None
            return result, output, str(root / "loading-title.rgba")

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required for the packaging integration cases")
    def test_cmake_records_exact_asset_destination(self):
        self.assertTrue(ASSET.is_file(), "The selected loading picture is missing")
        result, output, source = self.configure(ASSET.read_bytes())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, f"FILE;{source};sce_sys/loading-title.rgba")

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required for the packaging integration cases")
    def test_cmake_rejects_missing_or_wrong_size(self):
        for data in (None, b"", bytes(SIZE - 1), bytes(SIZE + 1)):
            with self.subTest(size=None if data is None else len(data)):
                result, output, _ = self.configure(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Loading artwork", result.stderr)
                self.assertIsNone(output)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required for the packaging integration cases")
    def test_cmake_rejects_wrong_picture_with_correct_size(self):
        result, output, _ = self.configure(bytes(SIZE))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Loading artwork SHA-256", result.stderr)
        self.assertIsNone(output)


if __name__ == "__main__":
    unittest.main()

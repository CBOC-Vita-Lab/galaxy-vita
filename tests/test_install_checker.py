# SPDX-License-Identifier: MIT
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CHECKER = Path(__file__).resolve().parents[1] / "tools/check-files.py"


class InstallCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = self.root / "Player build.vpk"
        self.payload.write_bytes(b"test package\n")
        self.digest = hashlib.sha256(self.payload.read_bytes()).hexdigest()
        self.manifest = self.root / "SHA256SUMS"
        self.manifest.write_text(self.digest + "  Player build.vpk\n")

    def run_checker(self):
        self.assertTrue(CHECKER.is_file(), "The offline bundle checker is missing")
        try:
            return subprocess.run(
                [sys.executable, str(CHECKER), "--directory", str(self.root)],
                capture_output=True, text=True, check=False, timeout=3,
            )
        except subprocess.TimeoutExpired:
            self.fail("Checker blocked on an input instead of rejecting it")

    def test_valid_bundle_with_spaces_passes_without_modifying_files(self):
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 file", result.stdout)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.root.iterdir()})

    def test_damaged_package_fails_and_is_not_repaired(self):
        self.payload.write_bytes(b"damaged")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Changed", result.stdout)
        self.assertEqual(self.payload.read_bytes(), b"damaged")

    def test_missing_package_fails(self):
        self.payload.unlink()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Missing", result.stdout)

    def test_missing_manifest_explains_full_extraction(self):
        self.manifest.unlink()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Extract the whole", result.stdout)

    def test_empty_manifest_cannot_report_success(self):
        self.manifest.write_text("\n")
        self.assertEqual(self.run_checker().returncode, 1)

    def test_malformed_digest_fails(self):
        self.manifest.write_text("not-a-digest  Player build.vpk\n")
        self.assertEqual(self.run_checker().returncode, 1)

    def test_unsafe_paths_are_rejected(self):
        for name in ("../outside", "/etc/hosts", "C:/outside", "dir\\outside", "dir/../outside"):
            with self.subTest(name=name):
                self.manifest.write_text(self.digest + "  " + name + "\n")
                result = self.run_checker()
                self.assertEqual(result.returncode, 1)
                self.assertIn("Unsafe", result.stdout)

    def test_duplicate_case_insensitive_names_fail(self):
        self.manifest.write_text(self.manifest.read_text() + self.digest + "  PLAYER BUILD.VPK\n")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Duplicate", result.stdout)

    def test_symlink_file_is_not_followed(self):
        self.payload.rename(self.root / "other.bin")
        self.payload.symlink_to("other.bin")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Link", result.stdout)

    def test_nested_regular_file_and_crlf_manifest_pass(self):
        folder = self.root / "docs"
        folder.mkdir()
        self.payload.rename(folder / self.payload.name)
        self.manifest.write_bytes((self.digest.upper() + "  docs/Player build.vpk\r\n").encode())
        self.assertEqual(self.run_checker().returncode, 0)

    def test_unlisted_user_files_are_not_modified(self):
        extra = self.root / "user.sav"
        extra.write_bytes(b"precious save")
        self.assertEqual(self.run_checker().returncode, 0)
        self.assertEqual(extra.read_bytes(), b"precious save")

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX named pipes required")
    def test_pipe_payload_fails_without_blocking(self):
        self.payload.unlink()
        os.mkfifo(self.payload)
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("regular file", result.stdout)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX named pipes required")
    def test_pipe_manifest_fails_without_blocking(self):
        self.manifest.unlink()
        os.mkfifo(self.manifest)
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("regular file", result.stdout)


if __name__ == "__main__":
    unittest.main()

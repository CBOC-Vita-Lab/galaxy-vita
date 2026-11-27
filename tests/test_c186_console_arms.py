import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import c186_console_arms as arms


class ConsoleArmTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def test_exact_one_change(self):
        for arm in arms.CHANGES:
            got = arms.preset(arm)
            self.assertEqual(got["GXV_VSOUT"], "1")
            self.assertEqual(sum(arms.BASE.get(k) != v for k, v in got.items()),
                             0 if arm == "baseline" else 1)
            self.assertTrue(set(arms.BASE) <= got.keys())

    def test_accepts_crlf_and_reordered_unique_lines(self):
        path = self.root / "env.txt"
        path.write_bytes(b"GXV_LA88_MIPS=1\r\nGXV_VSOUT=1")
        self.assertTrue(arms.check_env("baseline", path))
        self.assertFalse(arms.check_env("assist-off", path))

    def test_rejects_noncanonical_and_contaminated_env(self):
        path = self.root / "env.txt"
        base = b"GXV_VSOUT=1\nGXV_LA88_MIPS=1\n"
        for data in (base * 2, b"\xef\xbb\xbf" + base, base + b"GXV_FORCE=1",
                     base.replace(b"=1", b"= 1"), base.replace(b"=1", b" =1"),
                     base.replace(b"=1", b"=0"), base.replace(b"\n", b"\r"),
                     base + b"\n", base + b"#" * 4096):
            path.write_bytes(data)
            self.assertFalse(arms.check_env("baseline", path))

    def test_compare_snapshots(self):
        left, right = (self.root / name for name in ("a", "b"))
        for root in (left, right):
            root.mkdir()
            (root / "empty").mkdir()
            (root / "slot").write_bytes(b"01")
        self.assertTrue(arms.compare(left, right))
        (right / "slot").write_bytes(b"02")
        self.assertFalse(arms.compare(left, right))
        (right / "slot").write_bytes(b"01")
        (right / "empty").rmdir()
        self.assertFalse(arms.compare(left, right))
        (right / "slot").unlink()
        with self.assertRaises(ValueError):
            arms.compare(left, right)
        os.link(left / "slot", right / "alias")
        with self.assertRaises(ValueError):
            arms.compare(left, right)

    def test_rejects_same_tree_and_symlinks(self):
        left, right = (self.root / name for name in ("first", "second"))
        for root in (left, right):
            root.mkdir()
            (root / "slot").write_bytes(b"save")
        for a, b in ((left, left), (self.root, right)):
            with self.assertRaises(ValueError):
                arms.compare(a, b)
        (right / "cache").symlink_to(left)
        with self.assertRaises(ValueError):
            arms.compare(left, right)

    def test_zip_roundtrip_and_determinism(self):
        a, b = self.root / "one.zip", self.root / "two.zip"
        arms.kit(a)
        arms.kit(b)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        with self.assertRaises(FileExistsError):
            arms.kit(a)
        with zipfile.ZipFile(a) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(len(z.namelist()), 4 + len(arms.CHANGES))
            env = self.root / "env.txt"
            for arm in arms.CHANGES:
                env.write_bytes(z.read(f"env/{arm}/env.txt"))
                self.assertTrue(arms.check_env(arm, env))


if __name__ == "__main__":
    unittest.main()

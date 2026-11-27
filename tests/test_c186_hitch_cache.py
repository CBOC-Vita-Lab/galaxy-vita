from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import c186_hitch_cache as cache


class HostCache(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.source, self.base = [self.root.joinpath(x, *cache.TITLE_DIR) for x in ("donor", "base")]
        self.spv = "vk15-" + "a" * 64 + ".spv"
        self.out = self.root / "staged"
        for d in (self.source, self.base):
            d.mkdir(parents=True)
            self.pipeline(d)
            (d / cache.INDEX).write_bytes(struct.pack("<QII", 1, 15, 26) + bytes.fromhex("a"*64) + bytes(32))
            (d / self.spv).write_bytes(b"spv")

    def pipeline(self, path, driver=b"a"*16):
        (path / cache.PIPE).write_bytes(struct.pack("<IQQIIII16s", 0xbeef4321, 1, 42,
                                        32, 1, 7, 9, driver) + path.parts[-4].encode())

    def test_full_cache_is_staged_without_replacing_either_input(self):
        extra = "vk15-" + "b"*64 + ".spv"
        (self.source / extra).write_bytes(b"new")
        r = cache.make_host_overlay(self.source, self.base, self.out)
        self.assertEqual(r["files"], 4)
        self.assertFalse(r["guest_cache_imported"])
        self.assertEqual((self.out / cache.INDEX).read_bytes(), (self.source / cache.INDEX).read_bytes())
        self.assertFalse((self.base / extra).exists())
        self.assertEqual((self.out / cache.PIPE).read_bytes(), (self.source / cache.PIPE).read_bytes())
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)

    def test_single_file_is_not_a_coherent_checkpoint(self):
        with self.assertRaisesRegex(ValueError, "partial Vulkan caches"):
            cache.make_host_overlay(self.source, self.base, self.out, "pipeline")
        self.assertFalse(self.out.exists())

    def test_reject_driver_corruption_and_wrong_tree(self):
        self.pipeline(self.source, b"b"*16)
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)
        (self.source / cache.PIPE).write_bytes(b"damaged")
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.root, self.base, self.out)
        self.assertFalse(self.out.exists())

    def test_reject_link(self):
        (self.source / self.spv).unlink()
        (self.source / self.spv).symlink_to(self.base / self.spv)
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)

    def test_reject_changed_existing_shader(self):
        (self.source / self.spv).write_bytes(b"different code under same name")
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)
        self.assertFalse(self.out.exists())

    def test_reject_incoherent_index_and_feature_change(self):
        header = struct.pack("<QII", 1, 15, 26)
        (self.source / cache.INDEX).write_bytes(header + bytes.fromhex("b"*64) + bytes(32))
        with self.assertRaisesRegex(ValueError, "indexed shader missing"):
            cache.make_host_overlay(self.source, self.base, self.out)
        (self.source / cache.INDEX).write_bytes(struct.pack("<QII", 1, 15, 25) + bytes.fromhex("a"*64) + bytes(32))
        with self.assertRaisesRegex(ValueError, "feature masks"):
            cache.make_host_overlay(self.source, self.base, self.out)

    def test_reject_nested_output_and_non_cache_data(self):
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.base / "new")
        (self.source / "player-settings.cfg").write_text("not a cache")
        with self.assertRaises(ValueError):
            cache.make_host_overlay(self.source, self.base, self.out)


if __name__ == "__main__":
    unittest.main()

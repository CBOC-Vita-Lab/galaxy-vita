"""Tests for tools/cook/vita_layout.py: where the PS Vita build keeps each game file (bucket folders)."""
import os
from pathlib import Path
import struct
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "cook"))
import vita_layout as layout


def fst_bytes(tree):
    """A file table for `tree`: {dir name: {...}} for directories, an int (size) for files. Entry 0 is the root."""
    entries = []  # (is_dir, name, a, b)

    def walk(name, node, parent):
        index = len(entries)
        entries.append([True, name, parent, 0])
        for child_name, child in node.items():
            if isinstance(child, dict):
                walk(child_name, child, index)
            else:
                entries.append([False, child_name, 0, child])
        entries[index][3] = len(entries)

    walk("", tree, 0)
    strings = b""
    offsets = []
    for _is_dir, name, _a, _b in entries:
        offsets.append(len(strings))
        strings += name.encode("latin-1") + b"\0"
    out = b""
    for (is_dir, _name, a, b), off in zip(entries, offsets):
        out += struct.pack(">I", ((1 if is_dir else 0) << 24) | off) + struct.pack(">II", a, b)
    # the file table's root entry carries the number of entries in its last word
    out = out[:8] + struct.pack(">I", len(entries)) + out[12:]
    return out + strings


def make_tree(root, big=70, small=5):
    """A cooked tree with sys/fst.bin and flat files: one directory over the limit, one under, one in a sub-directory."""
    tree = {
        "Big": {"File%03d.arc" % i: 100 + i for i in range(big)},
        "Small": {"Item%d.arc" % i: 10 for i in range(small)},
        "Mixed": {"Sub": {"Deep%03d.arc" % i: 1 for i in range(big + 3)}, "Top.arc": 1},
    }
    os.makedirs(os.path.join(root, "sys"))
    with open(os.path.join(root, "sys", "fst.bin"), "wb") as f:
        f.write(fst_bytes(tree))
    for d, kids in tree.items():
        for k, v in kids.items():
            if isinstance(v, dict):
                for kk in v:
                    p = os.path.join(root, "files", d, k, kk)
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    Path(p).write_bytes(b"x")
            else:
                p = os.path.join(root, "files", d, k)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                Path(p).write_bytes(b"x")
    return tree


class BucketRuleTests(unittest.TestCase):
    def test_hash_is_fnv1a_of_the_lower_case_name(self):
        # FNV-1a 32 bit of "a": (2166136261 ^ 97) * 16777619 mod 2^32 = 0xE40C292C
        self.assertEqual(layout.bucket_of(b"a", 1 << 31), 0xE40C292C % (1 << 31))
        self.assertEqual(layout.bucket_of(b"MixedCase.ARC", 7), layout.bucket_of(b"mixedcase.arc", 7))
        self.assertEqual(layout.bucket_of(b"anything", 1), 0)

    def test_hash_matches_the_value_dvd_cpp_uses(self):
        # one name out of the real ObjectData layout, as dvd.cpp moved it on the console
        h = 2166136261
        for c in b"mario.arc":
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        self.assertEqual(layout.bucket_of(b"Mario.arc", 58), h % 58)


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.tree = make_tree(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def test_only_directories_over_the_limit_are_bucketed(self):
        moves, dirs = layout.layout(os.path.join(self.root, "sys", "fst.bin"))
        self.assertEqual(dict(dirs), {"Big": 3, "Mixed/Sub": 3})  # 70 -> ceil(70/32) = 3, 73 -> 3
        self.assertEqual(len(moves), 70 + 73)
        sources = {s for s, _d in moves}
        self.assertNotIn("files/Small/Item0.arc", sources)
        self.assertNotIn("files/Mixed/Top.arc", sources)

    def test_a_directory_of_exactly_the_limit_stays_flat(self):
        with tempfile.TemporaryDirectory() as t:
            make_tree(t, big=layout.MIN_FILES)
            moves, dirs = layout.layout(os.path.join(t, "sys", "fst.bin"))
            self.assertEqual(dict(dirs), {"Mixed/Sub": 3})
            self.assertFalse([m for m in moves if m[0].startswith("files/Big/")])

    def test_destination_is_the_bucket_of_the_name(self):
        moves, _dirs = layout.layout(os.path.join(self.root, "sys", "fst.bin"))
        for src, dst in moves:
            if src.startswith("files/Big/"):
                name = src.rsplit("/", 1)[1]
                self.assertEqual(dst, "files/Big/_bk%02u/%s" % (layout.bucket_of(name.encode(), 3), name))

    def test_apply_in_place_then_verify_then_flatten(self):
        moved, there, missing = layout.apply_in_place(self.root)
        self.assertEqual((moved, there, missing), (143, 0, 0))
        total, bad = layout.verify(self.root)
        self.assertEqual((total, bad), (143, []))
        self.assertFalse(os.path.exists(os.path.join(self.root, "files", "Big", "File000.arc")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "files", "Small", "Item0.arc")))
        # safe to run again
        self.assertEqual(layout.apply_in_place(self.root), (0, 143, 0))
        # and back
        self.assertEqual(layout.flatten(self.root), 143)
        self.assertTrue(os.path.exists(os.path.join(self.root, "files", "Big", "File000.arc")))
        self.assertFalse([d for d in os.listdir(os.path.join(self.root, "files", "Big")) if d.startswith("_bk")])
        total, bad = layout.verify(self.root)
        self.assertEqual(len(bad), 143)

    def test_a_second_cook_into_a_laid_out_tree_replaces_the_old_copies(self):
        layout.apply_in_place(self.root)
        flat = os.path.join(self.root, "files", "Big", "File005.arc")
        Path(flat).write_bytes(b"newer")  # what cook.py leaves after cooking again
        self.assertEqual(layout.apply_in_place(self.root), (1, 142, 0))
        dst = dict(layout.layout(os.path.join(self.root, "sys", "fst.bin"))[0])["files/Big/File005.arc"]
        self.assertEqual(Path(self.root, dst).read_bytes(), b"newer")
        self.assertFalse(os.path.exists(flat))

    def test_missing_files_are_counted_not_invented(self):
        os.remove(os.path.join(self.root, "files", "Big", "File003.arc"))
        moved, there, missing = layout.apply_in_place(self.root)
        self.assertEqual((moved, missing), (142, 1))

    def test_apply_copy_leaves_the_flat_tree_alone(self):
        with tempfile.TemporaryDirectory() as out:
            files, linked, copied = layout.apply_copy(self.root, out)
            self.assertEqual(files, linked + copied)
            self.assertEqual(layout.verify(self.root, out)[1], [])
            # the files outside the buckets are carried over where they were
            self.assertTrue(os.path.exists(os.path.join(out, "files", "Small", "Item1.arc")))
            self.assertTrue(os.path.exists(os.path.join(self.root, "files", "Big", "File000.arc")))
            self.assertTrue(os.path.exists(os.path.join(self.root, "sys", "fst.bin")))
            self.assertTrue(os.path.exists(os.path.join(out, "sys", "fst.bin")))

    def test_apply_copy_from_a_tree_that_is_already_laid_out(self):
        layout.apply_in_place(self.root)
        with tempfile.TemporaryDirectory() as out:
            layout.apply_copy(self.root, out, copy=True)
            self.assertEqual(layout.verify(self.root, out)[1], [])

    def test_a_folder_without_a_file_table_is_refused(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(SystemExit):
                layout.verify(t)


if __name__ == "__main__":
    unittest.main()

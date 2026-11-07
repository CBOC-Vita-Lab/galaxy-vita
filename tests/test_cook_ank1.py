"""Tests for the ANK1 (key animation, .bck) conversion in tools/cook/j3d.py: key counts of 0x8000 or more (GAL-018)."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "cook"))
sys.path.insert(0, str(ROOT / "tests"))
import j3d
import anm_key_fixture as fx

BLOCK = 0x20
MARKER_AT = BLOCK + 0x24


def le(fmt, b, o):
    return struct.unpack_from("<" + fmt, b, o)[0]


class Ank1Counts(unittest.TestCase):
    def test_counts_are_unsigned(self):
        # a rotation count of 40000 read as signed is -25536: the array used to stay big-endian
        be = fx.build_big_endian(33, 40000, 900)
        old, new = fx.convert(be, True), fx.convert(be, False)
        self.assertNotEqual(old, new)
        rot = le("I", new, BLOCK + 0x1C)
        self.assertEqual(rot % 2, 0)
        # old output: first rotation key time is still the big-endian bytes, new output: swapped
        first_be = struct.unpack_from(">h", be, BLOCK + rot)[0]
        self.assertEqual(le("h", new, BLOCK + rot), first_be)
        self.assertEqual(le("h", old, BLOCK + rot), struct.unpack("<h", struct.pack(">h", first_be))[0])
        # every other array and every header word agree between the two converters
        n = 40000 * 2
        a = bytearray(old)
        b = bytearray(new)
        a[BLOCK + rot:BLOCK + rot + n] = b[BLOCK + rot:BLOCK + rot + n] = bytes(n)
        a[MARKER_AT:MARKER_AT + 4] = b[MARKER_AT:MARKER_AT + 4] = b"\0\0\0\0"
        self.assertEqual(a, b)

    def test_each_array_kind_is_swapped(self):
        for name, counts in fx.CASES.items():
            if max(counts) < 0x8000:
                continue
            with self.subTest(case=name):
                new = fx.convert(fx.build_big_endian(*counts), False)
                self.assertEqual(le("H", new, BLOCK + 0x0E), counts[0])
                self.assertEqual(le("H", new, BLOCK + 0x10), counts[1])
                self.assertEqual(le("H", new, BLOCK + 0x12), counts[2])
                self.assertEqual(new[MARKER_AT:MARKER_AT + 4], j3d.ANK1_MARKER)
                # key times of every track, read little-endian, are 0, 1, 2, ...
                tbl, sc, ro, tr = [BLOCK + le("I", new, BLOCK + 0x14 + 4 * i) for i in range(4)]
                for e in range(9):
                    keys, off = le("H", new, tbl + 6 * e), le("H", new, tbl + 6 * e + 2)
                    for k in range(keys):
                        i = off + 3 * k
                        if e % 3 == 1:
                            self.assertEqual(le("h", new, ro + 2 * i), k)
                        else:
                            self.assertEqual(le("f", new, (sc if e % 3 == 0 else tr) + 4 * i), float(k))

    def test_small_counts_are_unchanged(self):
        for name, counts in fx.CASES.items():
            if max(counts) >= 0x8000:
                continue
            with self.subTest(case=name):
                be = fx.build_big_endian(*counts)
                old, new = fx.convert(be, True), fx.convert(be, False)
                self.assertEqual(old, new)  # no marker, same bytes as before the fix
                self.assertNotEqual(new[MARKER_AT:MARKER_AT + 4], j3d.ANK1_MARKER)

    def test_marker_only_replaces_padding(self):
        be = fx.build_big_endian(12, 2700, 39749)
        new = fx.convert(be, False)
        self.assertEqual(be[BLOCK + 0x24:BLOCK + 0x40], fx.PADDING)
        self.assertEqual(new[MARKER_AT:MARKER_AT + 4], b"UCNT")
        self.assertEqual(new[MARKER_AT + 4:BLOCK + 0x40], fx.PADDING[4:])

    def test_no_marker_without_room_for_it(self):
        be = fx.build_big_endian(12, 2700, 39749)
        struct.pack_into(">I", be, BLOCK + 0x14, 0x24)  # joint table offset inside the header
        new = bytearray(be)
        j3d.ank1(new, BLOCK, len(new))
        self.assertNotEqual(bytes(new[MARKER_AT:MARKER_AT + 4]), j3d.ANK1_MARKER)


if __name__ == "__main__":
    unittest.main()

"""fixnow31: tools/seedrun/stagetags.py package makes a COMPACT release VPK (about a dozen entries, one v3/seed.c182), checkvpk refuses a loose one.

Input: the test19 seed VPK (default /home/claude/w/in/test19/Galaxy-Vita-2.3-test19.vpk; STAGETAGS_SEED overrides; the seed tests skip without it) and a
STUB eboot (the real one is not needed). The base VPK is what the CMake smg-vgx.vpk holds: the five PNGs of vita/vgx-game/sce_sys (never regenerated), template.xml,
param.sfo, eboot.bin. The result is read back with a Python model of platform/vgx/vita/vgx_compact_seed.h (header, index crc, per-record crc) and every shader byte is compared.

    python3 -m unittest tests/test_stagetags_package.py
"""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile
from zlib import crc32

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/seedrun/stagetags.py"
SEED = Path(os.environ.get("STAGETAGS_SEED", "/home/claude/w/in/test19/Galaxy-Vita-2.3-test19.vpk"))
SYS = ROOT / "vita/vgx-game/sce_sys"
# git blob ids (first 8 hex) of the approved 3.0 artwork: icon0, pic0, bg, startup, settings
ART = {"sce_sys/icon0.png": "89b908c9", "sce_sys/pic0.png": "e533f76d", "sce_sys/livearea/contents/bg.png": "ccbcfb11",
       "sce_sys/livearea/contents/startup.png": "050a4dee", "sce_sys/livearea/contents/settings.png": "e124ce7e"}
PACKAGED = [n for n in ART if not n.endswith("settings.png")]   # CMakeLists.txt packages four; settings.png stays in the tree only


def blob(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()[:8]


def read_compact(image):
    """a model of vgx_compact_seed.h: -> {(kind, key): payload}; raises on a bad magic, index crc, record crc or bounds"""
    magic, version, count, abi, nbytes, tcrc = struct.unpack_from("<8sIIQII", image)
    assert magic == b"SMGSEED1" and version == 1
    table = image[32:32 + 24 * count]
    assert crc32(table) == tcrc and len(image) == 32 + 24 * count + nbytes
    body = image[32 + 24 * count:]
    out = {}
    for i in range(count):
        kind, size, key, off, crc = struct.unpack_from("<IIQII", table, 24 * i)
        payload = body[off:off + size]
        assert len(payload) == size and crc32(payload) == crc and (kind, key) not in out
        out[(kind, key)] = payload
    return out


def run(*args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True)


def make_base(path):
    with ZipFile(path, "w") as z:
        z.writestr("eboot.bin", b"stub eboot")
        z.writestr("sce_sys/param.sfo", b"stub sfo")
        z.writestr("sce_sys/livearea/contents/template.xml", (SYS / "livearea/contents/template.xml").read_bytes())
        for n in PACKAGED:
            z.writestr(n, (SYS / n[len("sce_sys/"):]).read_bytes())


class Art(unittest.TestCase):
    def test_tree_art_is_the_contributor_art(self):
        for n, want in ART.items():
            self.assertEqual(blob((SYS / n[len("sce_sys/"):]).read_bytes()), want, n)


@unittest.skipUnless(SEED.is_file(), "the test19 seed VPK is not here")
class Compact(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        d = Path(cls.td.name)
        cls.base, cls.out = d / "base.vpk", d / "out.vpk"
        make_base(cls.base)
        cls.r = run("package", cls.base, SEED, cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.td.cleanup()

    def test_package_ok_and_small(self):
        self.assertEqual(self.r.returncode, 0, self.r.stdout + self.r.stderr)
        self.assertIn("compact seed:", self.r.stdout)
        with ZipFile(self.out) as z:
            names = z.namelist()
        self.assertLessEqual(len(names), 16, names)
        self.assertFalse([n for n in names if n.endswith(".gxp")])
        self.assertEqual(sum(n.endswith("/v3/seed.c182") for n in names), 1)

    def test_check_passes_and_art_is_byte_identical(self):
        r = run("checkvpk", self.out)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with ZipFile(self.out) as z:
            for n in PACKAGED:
                self.assertEqual(blob(z.read(n)), ART[n], n)

    def test_read_back_every_shader_byte(self):
        with ZipFile(SEED) as zs, ZipFile(self.out) as zo:
            pat = re.compile(r"shadercache/[0-9a-f]{8}/v3/([vf])/[0-9A-F]{2}/([0-9A-F]{1,16})\.gxp")
            orig = {}
            for i in zs.infolist():
                m = pat.fullmatch(i.filename)
                if m:
                    orig[(1 if m.group(1) == "v" else 2, int(m.group(2), 16))] = zs.read(i)
            seed, = (n for n in zo.namelist() if n.endswith("/v3/seed.c182"))
            got = read_compact(zo.read(seed))
            self.assertEqual(len(orig), 5174)
            self.assertEqual(got, orig)
            # the other seed members pass through unchanged, except the stage list (tagged)
            for n in ("shadercache/vgx_shaderlist.bin", "shadercache/b4d4c2a9/stages.pack"):
                self.assertEqual(zo.read(n), zs.read(n), n)
            self.assertNotEqual(zo.read("shadercache/vgx_stagewarm.bin"), zs.read("shadercache/vgx_stagewarm.bin"))

    def test_loose_seed_vpk_fails_the_hard_check(self):
        # the donor itself: tagged by the old path it still has 5,185 loose members
        tagged = Path(self.td.name) / "loose.vpk"
        r = run("package", self.base, SEED, tagged, "--loose")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = run("checkvpk", tagged)
        self.assertEqual(r.returncode, 1)
        self.assertIn("members (limit 64)", r.stdout)
        self.assertIn("loose shader files", r.stdout)
        self.assertEqual(run("checkvpk", tagged, "--loose").returncode, 0)

    def test_over_64_members_fails_even_without_loose_shaders(self):
        fat = Path(self.td.name) / "fat.vpk"
        with ZipFile(self.out) as zi, ZipFile(fat, "w") as zo:
            for i in zi.infolist():
                zo.writestr(i, zi.read(i))
            for k in range(60):
                zo.writestr("extra/%d.bin" % k, b"x")
        r = run("checkvpk", fat)
        self.assertEqual(r.returncode, 1)
        self.assertIn("members (limit 64)", r.stdout)

    def test_oversized_shader_is_refused_not_left_loose(self):
        # strict compact: a payload over the reader's record limit must fail the package
        seedd = Path(self.td.name) / "seed/shadercache/b4d4c2a9"
        (seedd / "v3/v/00").mkdir(parents=True)
        pack = bytearray(40); pack[:8] = b"VGXRAM01"
        (seedd / "stages.pack").write_bytes(bytes(pack))
        (seedd / "v3/v/00/1.gxp").write_bytes(b"x" * 5000)
        with ZipFile(SEED) as z:
            (seedd.parent / "vgx_stagewarm.bin").write_bytes(z.read("shadercache/vgx_stagewarm.bin"))
        r = run("package", self.base, seedd.parent, Path(self.td.name) / "o2.vpk")
        self.assertEqual(r.returncode, 1)
        self.assertIn("would stay loose", r.stdout + r.stderr)
        self.assertFalse((Path(self.td.name) / "o2.vpk").exists())


if __name__ == "__main__":
    unittest.main()

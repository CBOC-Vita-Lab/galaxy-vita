"""Host test of tools/check_weak_links.py (linkfix31): a weak-only reference to an archive member, a weak default that beats an unlinked
strong one, the must-define list, the allow list's reasons and the --build-dir reading of build.ninja. Builds tiny programs with the host
C compiler (and with the VitaSDK's GCC when it is installed), so the archive rules are the real linker's.

    python3 -m unittest tests/test_check_weak_links.py      (or: python3 tests/test_check_weak_links.py)
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/check_weak_links.py"
VITASDK = Path(os.environ.get("VITASDK", "/opt/vitasdk"))

MAIN = r"""
extern int hook_a(void) __attribute__((weak));      /* only weak references: the archive member is never pulled */
int hook_b(void) __attribute__((weak));
int hook_b(void) { return 0; }                     /* a weak default; a strong one sits in an archive member nobody pulls */
extern int must_x(void);                            /* a strong reference: pulled */
int main(void) { return (hook_a ? hook_a() : 0) + hook_b() + must_x(); }
"""
HOOK_A = "int hook_a(void) { return 1; }\n"
HOOK_B = "int hook_b(void) { return 2; }\n"
MUST_X = "int must_x(void) { return 3; }\n"


class Toolchain:
    def __init__(self, cc, ar, nm, readelf, cflags=(), ldflags=()):
        self.cc, self.ar, self.nm, self.readelf, self.cflags, self.ldflags = cc, ar, nm, readelf, list(cflags), list(ldflags)

    def available(self):
        return all(shutil.which(t) or Path(t).is_file() for t in (self.cc, self.ar, self.nm, self.readelf))


HOST = Toolchain(os.environ.get("CC", "cc"), "ar", "nm", "readelf")
VITA = Toolchain(str(VITASDK / "bin/arm-vita-eabi-gcc"), str(VITASDK / "bin/arm-vita-eabi-ar"), str(VITASDK / "bin/arm-vita-eabi-nm"),
                 str(VITASDK / "bin/arm-vita-eabi-readelf"), ldflags=["-Wl,-q"])


class WeakLinkTests(unittest.TestCase):
    tc = HOST

    def setUp(self):
        if not self.tc.available():
            self.skipTest("toolchain not installed: " + self.tc.cc)
        self.dir = Path(tempfile.mkdtemp(prefix="weaklinks."))
        for name, text in (("main.c", MAIN), ("hook_a.c", HOOK_A), ("hook_b.c", HOOK_B), ("must_x.c", MUST_X)):
            (self.dir / name).write_text(text)
            self.sh([self.tc.cc, *self.tc.cflags, "-O1", "-c", name, "-o", name[:-2] + ".o"])
        self.sh([self.tc.ar, "rcs", "libhooks.a", "hook_a.o", "hook_b.o", "must_x.o"])
        (self.dir / "allow.txt").write_text("# test allow list\n")
        (self.dir / "must.txt").write_text("must_x   # pulled by a strong reference\nhook_a   # must be linked\n")

    def tearDown(self):
        if hasattr(self, "dir"):
            shutil.rmtree(self.dir, ignore_errors=True)

    def sh(self, args):
        subprocess.run(args, cwd=self.dir, check=True, capture_output=True)

    def link(self, out, extra=()):
        self.sh([self.tc.cc, *self.tc.ldflags, "main.o", "libhooks.a", *extra, "-o", out])

    def check(self, elf, *args):
        r = subprocess.run([sys.executable, "-I", str(TOOL), elf, "--nm", self.tc.nm, "--readelf", self.tc.readelf, *map(str, args)],
                           cwd=self.dir, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def test_finds_left_out_and_weak_won(self):
        self.link("bad")
        rc, out = self.check("bad", "--inputs", "libhooks.a", "--allow", "allow.txt", "--must-define", "must.txt")
        self.assertEqual(rc, 1, out)
        self.assertIn("FAIL left-out: hook_a: defined in libhooks.a(hook_a.o)", out)
        self.assertIn("FAIL weak-won: hook_b", out)
        self.assertIn("FAIL must-define: hook_a", out)
        self.assertNotIn("must-define: must_x", out)
        self.assertIn("must-define: 1/2 defined strongly", out)

    def test_old_mode_without_inputs_still_fails_on_weak_undefined(self):
        self.link("bad")
        rc, out = self.check("bad", "--allow", "allow.txt")
        self.assertEqual(rc, 1, out)
        self.assertIn("FAIL undefined-weak: hook_a", out)

    def test_require_defined_fixes_it(self):
        self.link("good", ["-Wl,--require-defined=hook_a"])
        rc, out = self.check("good", "--inputs", "libhooks.a", "--allow", "allow.txt", "--must-define", "must.txt")
        self.assertEqual(rc, 1, out)                     # hook_b: the weak default still wins (--require-defined cannot fix that)
        self.assertNotIn("hook_a", "\n".join(l for l in out.splitlines() if l.startswith("FAIL")))
        self.assertIn("FAIL weak-won: hook_b", out)
        (self.dir / "allow.txt").write_text("hook_b   # test: the weak default is the intended one here\n")
        rc, out = self.check("good", "--inputs", "libhooks.a", "--allow", "allow.txt", "--must-define", "must.txt")
        self.assertEqual(rc, 0, out)
        self.assertIn("RESULT: OK", out)

    def test_must_define_wants_a_global_definition(self):
        # (c186merge, from C186.1's Settings gate) a LOCAL definition of the name (nm t) cannot satisfy another unit's reference
        (self.dir / "local.c").write_text("__attribute__((used, noinline)) static int must_l(void) { return 4; }\n"
                                          "int use_l(void) { return must_l(); }\n")
        self.sh([self.tc.cc, *self.tc.cflags, "-O0", "-c", "local.c", "-o", "local.o"])
        self.link("good", ["-Wl,--require-defined=hook_a", "local.o", "-Wl,--undefined=use_l"])
        (self.dir / "must.txt").write_text("must_x   # pulled by a strong reference\nmust_l   # only a local definition exists\n")
        rc, out = self.check("good", "--must-define", "must.txt", "--allow", "allow.txt")
        self.assertIn("FAIL must-define: must_l: nm type t in the ELF", out)
        self.assertNotIn("must-define: must_x", out)
        self.assertIn("must-define: 1/2 defined strongly", out)

    def test_allow_needs_a_reason(self):
        self.link("bad")
        (self.dir / "allow.txt").write_text("hook_a\n")
        rc, out = self.check("bad", "--inputs", "libhooks.a", "--allow", "allow.txt")
        self.assertEqual(rc, 2, out)
        self.assertIn("has no reason", out)

    def test_allowed_left_out_passes(self):
        self.link("bad")
        (self.dir / "allow.txt").write_text("hook_a  # optional\nhook_b  # optional\n")
        rc, out = self.check("bad", "--inputs", "libhooks.a", "--allow", "allow.txt")
        self.assertEqual(rc, 0, out)

    def test_build_dir_reads_build_ninja_and_remaps_the_recorded_root(self):
        self.link("bad")
        b = self.dir / "b"
        (b / "vita-vgx-runtime").mkdir(parents=True)
        (b / "vita-vgx-game").mkdir()
        shutil.copy(self.dir / "libhooks.a", b / "vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a")
        (b / "vita-vgx-game/build.ninja").write_text(
            "build galaxy_native: CXX_EXECUTABLE_LINKER__galaxy_native_Release main.o\n"
            "  LINK_FLAGS = -Wl,--gc-sections\n"
            "  LINK_LIBRARIES = -Wl,--start-group  /recorded-root/vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a  libsmg_net.a  "
            "-Wl,--end-group  /elsewhere/libx.a  -lm\n"
            "build eboot.bin: phony galaxy_native\n")
        rc, out = self.check("bad", "--build-dir", b, "--allow", "allow.txt")
        self.assertEqual(rc, 1, out)
        self.assertIn("FAIL left-out: hook_a: defined in libgalaxy_vita_runtime_vgx.a(hook_a.o)", out)
        self.assertIn("NOT FOUND", out)                  # libsmg_net.a and /elsewhere/libx.a do not exist: named, not fatal
        self.assertIn("/elsewhere/libx.a", out)

    def test_repo_lists_parse(self):
        rc, out = 0, ""
        for name in ("tools/weak_links.allow", "tools/vita_must_define.txt"):
            for i, line in enumerate((ROOT / name).read_text().splitlines(), 1):
                body, _, reason = line.partition("#")
                if body.strip():
                    self.assertEqual(len(body.split()), 1, "%s:%d: one name per line" % (name, i))
                    if name.endswith(".allow"):
                        self.assertTrue(reason.strip(), "%s:%d: an allowed name needs a reason" % (name, i))
        must = (ROOT / "tools/vita_must_define.txt").read_text()
        for s in ("port_settings_native_available", "port_less_motion_enabled", "port_less_motion_set", "port_tilt_stick_enabled",
                  "port_tilt_stick_set", "port_tilt_level", "port_tilt_level_set", "port_settings_native_request"):
            self.assertIn("\n" + s + " ", must)
        # fixnow31: the integ-3.1 weak hooks ("Save and Start Again" needs the first five) must be in the must-define list
        for s in ("port_nand_progress", "port_nand_failures", "port_card_write_finished", "port_card_write_failures", "port_nand_all_atomic",
                  "vgxvita_asyncWorkerTid", "vgxvita_workerAllocStats_v1", "port_pose_blendfix_turn_begin", "port_bowl_pose_turn_begin", "port_pose_ondraw_turn_begin",
                  "vgx_tex_decode_counts", "vgxvita_swizzleLeftPending", "vgxvita_texupCensusLine", "port_ptcl_pre_mode"):
            self.assertIn("\n" + s + " ", must)
        cmake = (ROOT / "vita/vgx-game/CMakeLists.txt").read_text()
        for s in ("port_settings_native_available", "port_less_motion_enabled", "port_less_motion_set", "port_tilt_stick_enabled",
                  "port_tilt_stick_set", "port_tilt_level", "port_tilt_level_set", "port_settings_hint_visible", "port_settings_native_request"):
            self.assertIn('"-Wl,--require-defined=%s"' % s, cmake)
        build = (ROOT / "tools/vgx_build/build.py").read_text()
        self.assertIn("link_check(build, env", build)
        self.assertIn("tools/vita_must_define.txt", build)


class VitaWeakLinkTests(WeakLinkTests):
    tc = VITA

    def test_repo_lists_parse(self):
        pass


if __name__ == "__main__":
    unittest.main()

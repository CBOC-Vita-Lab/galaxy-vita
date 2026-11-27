"""C186.1's Settings link gate, run through the general mechanism (c186merge): tools/check_weak_links.py --must-define
tools/vita_must_define.txt, the same check tools/vgx_build/build.py runs after every Vita link (linkfix31).

C186.1 checked nine Settings hooks with its own list (tools/release_c186/settings-link-symbols.txt) and asked for strong GLOBAL text.
On integ-3.1 the list is the "native Settings" block of tools/vita_must_define.txt (a superset: it also names the two NativeHost
functions) and check_weak_links.py's must-define check asks for a global strong definition. Real ARM ELF objects, no Vita SDK needed.
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
MUST = ROOT / "tools/vita_must_define.txt"
C186_SETTINGS = ("port_settings_native_available", "port_less_motion_enabled", "port_less_motion_set", "port_tilt_stick_enabled",
                 "port_tilt_stick_set", "port_tilt_level", "port_tilt_level_set", "port_settings_native_request",
                 "port_settings_hint_visible")


def settings_block():
    """The names of the `native Settings` block of tools/vita_must_define.txt (up to the next `# ---` header)."""
    names, inside = [], False
    for line in MUST.read_text().splitlines():
        if line.startswith("# ---"):
            inside = "native Settings" in line
            continue
        body = line.partition("#")[0].strip()
        if inside and body:
            names.append(body.split()[0])
    return names


class SettingsLink(unittest.TestCase):
    def test_list_covers_c186(self):
        names = settings_block()
        self.assertEqual(len(names), len(set(names)))
        self.assertFalse(set(C186_SETTINGS) - set(names), "a Settings hook C186.1 checked is missing from tools/vita_must_define.txt")
        cmake = (ROOT / "vita/vgx-game/CMakeLists.txt").read_text()
        for s in C186_SETTINGS:
            self.assertIn('"-Wl,--require-defined=%s"' % s, cmake)
        self.assertNotIn("settings-link-symbols.txt", cmake)

    def test_global_strong_only(self):
        clang = os.environ.get("CLANG", "clang")
        nm = os.environ.get("NM", "llvm-nm" if shutil.which("llvm-nm") else "nm")
        readelf = os.environ.get("READELF", "llvm-readelf" if shutil.which("llvm-readelf") else "readelf")
        if not all(shutil.which(t) for t in (clang, nm, readelf)):
            self.skipTest("clang / nm / readelf not installed")
        names = settings_block()
        with tempfile.TemporaryDirectory() as temp:
            obj = Path(temp) / "settings.o"
            must = Path(temp) / "must.txt"
            must.write_text("".join("%s  # Settings\n" % n for n in names))

            def check(source):
                subprocess.run([clang, "--target=arm-none-eabi", "-O0", "-x", "c", "-c", "-", "-o", str(obj)],
                               input=source, text=True, check=True)
                r = subprocess.run([sys.executable, "-I", str(TOOL), str(obj), "--nm", nm, "--readelf", readelf,
                                    "--must-define", str(must)], capture_output=True, text=True)
                return r.returncode, r.stdout + r.stderr

            def definitions(symbols):
                return "\n".join("int %s(void) { return 0; }" % s for s in symbols)

            rc, out = check(definitions(names))
            self.assertEqual(rc, 0, out)
            self.assertIn("must-define: %d/%d defined strongly" % (len(names), len(names)), out)
            # C186's original ELF: an unresolved weak reference, a weak implementation; and a missing hook and a LOCAL one (static: nm t),
            # which cannot satisfy another unit's reference.
            weak, request, local, missing = "port_settings_native_available", "port_settings_native_request", "port_less_motion_enabled", \
                "port_tilt_level_set"
            rc, out = check(definitions(n for n in names if n not in (weak, request, local, missing))
                            + "\nextern int %s(void) __attribute__((weak));" % weak
                            + "\nint call(void) { return %s ? %s() : 0; }" % (weak, weak)
                            + "\n__attribute__((weak)) int %s(void) { return 0; }" % request
                            + "\n__attribute__((used, noinline)) static int %s(void) { return 1; }" % local
                            + "\nint call_local(void) { return %s(); }" % local)
            self.assertEqual(rc, 1, out)
            failed = {line.split()[2].rstrip(":") for line in out.splitlines() if line.startswith("FAIL must-define:")}
            self.assertEqual(failed, {weak, request, local, missing}, out)


if __name__ == "__main__":
    unittest.main()

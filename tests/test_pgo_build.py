import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools/vgx_build"
PROFDATA = shutil.which("llvm-profdata") or shutil.which("llvm-profdata-18")

TEXT = """:ir
{root}/decomp/src/Game/a.cpp;_ZL1fv
# Func Hash:
942389666994816328
# Num Counters:
3
# Counter Values:
1000
1
500

_ZN4Game1gEv
# Func Hash:
742261418966908927
# Num Counters:
1
# Counter Values:
7

"""


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_profile(directory, recorded_root):
    text = Path(directory) / "p.txt"
    text.write_text(TEXT.format(root=recorded_root))
    out = Path(directory) / "p.profdata"
    subprocess.run([PROFDATA, "merge", "-o", out, text], check=True)
    return out


def show_text(profile):
    return subprocess.run([PROFDATA, "merge", "--text", "-o", "-", profile], capture_output=True, text=True,
                          check=True).stdout


class PgoBuildTests(unittest.TestCase):
    def setUp(self):
        self.builder = load("vgx_build_pgo", TOOLS / "build.py")

    def test_raw_or_text_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            for content in (b"\x81Rforpl\xff" + bytes(64), b":ir\nfoo\n"):
                profile = Path(directory) / "p"
                profile.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "not an indexed profile"):
                    self.builder.prepare_profile(profile, Path("/src"), Path(directory) / "pgo", dict(os.environ))

    @unittest.skipUnless(PROFDATA, "llvm-profdata not installed")
    def test_profile_of_this_checkout_is_copied_under_a_content_name(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = make_profile(directory, "/work/checkout")
            with redirect_stdout(io.StringIO()):
                out = self.builder.prepare_profile(profile, Path("/work/checkout"), Path(directory) / "pgo",
                                                   dict(os.environ))
            self.assertEqual(out.read_bytes(), profile.read_bytes())
            self.assertRegex(out.name, r"^[0-9a-f]{12}\.profdata$")

    @unittest.skipUnless(PROFDATA, "llvm-profdata not installed")
    def test_profile_of_another_checkout_names_its_static_functions_here(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = make_profile(directory, "/recorded/elsewhere")
            with redirect_stdout(io.StringIO()):
                out = self.builder.prepare_profile(profile, Path("/work/checkout"), Path(directory) / "pgo",
                                                   dict(os.environ))
            text = show_text(out)
            self.assertIn("/work/checkout/decomp/src/Game/a.cpp;_ZL1fv", text)
            self.assertNotIn("/recorded/elsewhere", text)
            self.assertIn("_ZN4Game1gEv", text)           # the external-linkage function is untouched
            self.assertIn("1000\n1\n500", text)           # counters survive the round trip
            self.assertIn(":ir", text)                    # still an IR-level profile
            self.assertFalse((Path(directory) / "pgo/profile.txt").exists())

    def test_source_stage_passes_the_profile_to_cmake_only_when_asked(self):
        calls = []

        def fake_run(args, env, log=None, cwd=None):
            calls.append([str(a) for a in args])
            return "clang version 18.1.3" if args[0] == "clang" else ""

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            root, sdk, deps, build = (base / n for n in ("source", "sdk", "deps", "build"))
            here = root / "tools/vgx_build"
            for path in (here, sdk, deps, root / "docs", build):
                path.mkdir(parents=True)
            (here / "inputs.json").write_text(json.dumps({"sdk": [], "dependencies": []}))
            (root / "docs/SOURCE-MANIFEST.json").write_text(json.dumps({"files": []}))
            profile = base / "in.profdata"
            profile.write_bytes(self.builder.PROFDATA_MAGIC + bytes(32))
            with patch.object(self.builder, "ROOT", root), patch.object(self.builder, "HERE", here), \
                    patch.object(self.builder, "run", fake_run), \
                    patch.dict(os.environ, {"VITASDK": str(sdk), "VGL_DEPS": str(deps)}):
                for extra in ([], ["--pgo-profile", str(profile)]):
                    del calls[:]
                    with patch.object(sys, "argv", ["build.py", "source", "--build-dir", str(build)] + extra), \
                            redirect_stdout(io.StringIO()):
                        self.builder.main()
                    configure = next(c for c in calls if c[0] == "cmake")
                    flag = next(a for a in configure if a.startswith("-DVITA_PGO_PROFILE="))
                    if extra:
                        self.assertRegex(flag, r"/build/pgo/[0-9a-f]{12}\.profdata$")
                        self.assertTrue(Path(flag.split("=", 1)[1]).is_file())
                    else:
                        self.assertEqual(flag, "-DVITA_PGO_PROFILE=")
                with patch.object(sys, "argv", ["build.py", "source", "--build-dir", str(build),
                                                "--pgo-profile", str(base / "missing")]):
                    with self.assertRaisesRegex(ValueError, "no such file"):
                        self.builder.main()

    def test_runtime_units_do_not_inherit_the_game_profile(self):
        gen = load("vgx_runtime_gen_pgo", TOOLS / "vita_runtime_gen_vgx.py")
        entry = {"arguments": ["clang++", "-O2", "-fprofile-use=/b/pgo/x.profdata", "-MD", "-c", "a.cpp", "-o", "a.o",
                               "-fprofile-instr-use=/b/y.profdata", "-Wall"]}
        self.assertEqual(gen.base_command(entry), ["clang++", "-O2", "-Wall"])


if __name__ == "__main__":
    unittest.main()

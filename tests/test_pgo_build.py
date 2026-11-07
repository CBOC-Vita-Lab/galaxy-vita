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

{root}/platform/vgx/src/vgx_draw.cpp;_ZN3vgxL1fEv
# Func Hash:
942389666994816329
# Num Counters:
2
# Counter Values:
40
2

{root}/platform/src/gx/gx_fifo.cpp;_ZL1gv
# Func Hash:
942389666994816330
# Num Counters:
1
# Counter Values:
9

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
    def test_gzipped_profile_is_unpacked(self):
        import gzip
        with tempfile.TemporaryDirectory() as directory:
            profile = make_profile(directory, "/work/checkout")
            packed = Path(directory) / "p.profdata.gz"
            packed.write_bytes(gzip.compress(profile.read_bytes()))
            with redirect_stdout(io.StringIO()):
                out = self.builder.prepare_profile(packed, Path("/work/checkout"), Path(directory) / "pgo",
                                                   dict(os.environ))
            self.assertEqual(out.read_bytes(), profile.read_bytes())
            with redirect_stdout(io.StringIO()):
                moved = self.builder.prepare_profile(packed, Path("/elsewhere"), Path(directory) / "pgo2",
                                                     dict(os.environ))
            self.assertIn("/elsewhere/decomp/src/Game/a.cpp;_ZL1fv", show_text(moved))

    @unittest.skipUnless(PROFDATA, "llvm-profdata not installed")
    def test_profile_of_another_checkout_names_its_static_functions_here(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = make_profile(directory, "/recorded/elsewhere")
            with redirect_stdout(io.StringIO()):
                out = self.builder.prepare_profile(profile, Path("/work/checkout"), Path(directory) / "pgo",
                                                   dict(os.environ))
            text = show_text(out)
            self.assertIn("/work/checkout/decomp/src/Game/a.cpp;_ZL1fv", text)
            # the renderer's units (runtime stage) are named the same way
            self.assertIn("/work/checkout/platform/vgx/src/vgx_draw.cpp;_ZN3vgxL1fEv", text)
            self.assertIn("/work/checkout/platform/src/gx/gx_fifo.cpp;_ZL1gv", text)
            self.assertNotIn("/recorded/elsewhere", text)
            self.assertIn("_ZN4Game1gEv", text)           # the external-linkage function is untouched
            self.assertIn("1000\n1\n500", text)           # counters survive the round trip
            self.assertIn(":ir", text)                    # still an IR-level profile
            self.assertFalse((Path(directory) / "pgo/profile.txt").exists())

    def test_runtime_generator_profiles_only_the_renderer_units(self):
        gen = TOOLS / "vita_runtime_gen_vgx.py"
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            root, source_build, out, deps = (base / n for n in ("root", "src-build", "out", "deps"))
            for rel in ("platform/vgx/src/vgx_draw.cpp", "platform/vgx/src/vgx_backend_null.cpp",
                        "platform/src/gx/gx_fifo.cpp", "platform/src/gx/vita_efb.cpp", "platform/src/audio/ai_dsp.cpp",
                        "platform/src/gx/readme.c", "vita/runtime/flat_view.cpp"):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("")
            source_build.mkdir()
            (source_build / "compile_commands.json").write_text(json.dumps([
                {"directory": str(root), "file": str(root / "decomp/x.cpp"), "command": f"clang++ -O2 -fprofile-use=/old.profdata -c x.cpp -o x.o"},
                {"directory": str(root), "file": str(root / "decomp/y.c"), "command": "clang -O2 -c y.c -o y.o"}]))
            profile = base / "p.profdata"
            profile.write_bytes(b"\xfflprofi\x81")
            env = {k: v for k, v in os.environ.items() if not k.startswith("VITA_")}
            for with_profile in (False, True):
                run_env = dict(env, VITA_PGO_PROFILE=str(profile)) if with_profile else env
                subprocess.run([sys.executable, "-I", gen, root, source_build, out, deps, "t"], env=run_env, check=True,
                               capture_output=True)
                lines = (out / "build.ninja").read_text().splitlines()
                cmds = {}
                for i, line in enumerate(lines):
                    if line.startswith("build ") and ": cc " in line:
                        cmds[line.split(": cc ")[1]] = lines[i + 1] + lines[i + 2]
                used = sorted(Path(k).relative_to(root).as_posix() for k, v in cmds.items() if "-fprofile-use=" in v.split("extra =")[-1])
                self.assertTrue(all("-fprofile-use=/old" not in v for v in cmds.values()))     # the game units' profile is stripped
                if with_profile:
                    self.assertEqual(used, ["platform/src/gx/gx_fifo.cpp", "platform/vgx/src/vgx_draw.cpp"])
                else:
                    self.assertEqual(used, [])

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

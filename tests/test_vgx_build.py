import importlib.util
from contextlib import contextmanager, redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools/vgx_build"


class VgxBuildTests(unittest.TestCase):
    def run_tool(self, name, *args, **kwargs):
        return subprocess.run([sys.executable, str(TOOLS / name), *map(str, args)],
                              capture_output=True, text=True, **kwargs)

    def test_missing_sdk_fails_before_creating_build_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            env = dict(os.environ, VITASDK=str(Path(directory) / "missing"),
                       VGL_DEPS=str(Path(directory) / "deps"))
            result = self.run_tool("build.py", "check", "--build-dir", output, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("VITASDK", result.stderr)
            self.assertFalse(output.exists())

    def test_case_shim_preserves_existing_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            shim = Path(directory) / "shim"
            shim.mkdir()
            sentinel = shim / "keep.txt"
            sentinel.write_text("keep")
            result = self.run_tool("make_case_shim.py", ROOT, shim)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("already exists", result.stderr)
            self.assertEqual(sentinel.read_text(), "keep")

    def test_case_shim_creates_only_mismatched_include_alias(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            header = root / "decomp/include/Game/Example.h"
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n")
            source = root / "decomp/example.cpp"
            source.write_text('#include "Game/example.h"\n')
            shim = Path(directory) / "shim"
            result = self.run_tool("make_case_shim.py", root, shim)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((shim / "Game/example.h").resolve(), header.resolve())
            self.assertEqual(header.read_text(), "#pragma once\n")

    def test_incomplete_runtime_cannot_replace_an_existing_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "inventory.tsv").write_text(
                "source\tselection\tgroup\treason\n"
                "platform/missing.cpp\tattempt\tgame-int32\t\n")
            archive = out / "libgalaxy_vita_runtime_vgx.a"
            archive.write_bytes(b"previous archive")
            result = self.run_tool("vita_runtime_report_vgx.py", ROOT, out)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAILED", result.stdout)
            self.assertEqual(archive.read_bytes(), b"previous archive")

    def load_builder(self):
        path = TOOLS / "build.py"
        self.assertTrue(path.is_file(), "Portable build driver is missing")
        spec = importlib.util.spec_from_file_location("vgx_build", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    @contextmanager
    def build_fixture(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            root, sdk, deps, build = (base / name for name in ("source", "sdk", "deps", "build"))
            here = root / "tools/vgx_build"
            for path in (here, sdk, deps, build):
                path.mkdir(parents=True)
            (here / "inputs.json").write_text(json.dumps({"sdk": [], "dependencies": []}))
            (root / "docs").mkdir()
            (root / "docs/SOURCE-MANIFEST.json").write_text(json.dumps({"files": []}))
            artifacts = builder.ARTIFACTS
            for name in artifacts.values():
                path = build / name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(b"abc")
            expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
            (root / "docs/BUILD-VERIFICATION.json").write_text(json.dumps({
                "sha256": dict.fromkeys(("runtimeArchive", "elf", "velf", "runtime"), expected),
            }))
            with patch.object(builder, "ROOT", root), patch.object(builder, "HERE", here), \
                    patch.dict(os.environ, {"VITASDK": str(sdk), "VGL_DEPS": str(deps)}):
                def verify(output=build):
                    with patch.object(sys, "argv", ["build.py", "verify", "--build-dir", str(output)]), \
                            redirect_stdout(io.StringIO()):
                        builder.main()
                yield root, sdk, deps, build, verify

    def test_runtime_output_symlink_cannot_redirect_into_protected_inputs(self):
        for target_name in ("source", "sdk", "deps"):
            with self.subTest(target=target_name), self.build_fixture() as fixture:
                root, sdk, deps, build, verify = fixture
                target = {"source": root, "sdk": sdk, "deps": deps}[target_name]
                name = "libgalaxy_vita_runtime_vgx.a"
                (build / "vita-vgx-runtime" / name).unlink()
                (build / "vita-vgx-runtime").rmdir()
                (target / name).write_bytes(b"abc")
                (build / "vita-vgx-runtime").symlink_to(target, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, "symlink"):
                    verify()
                self.assertEqual((target / name).read_bytes(), b"abc")

    def test_nested_output_and_log_symlinks_are_rejected(self):
        for name in ("logs/source.log", "vita-source/nested", "vita-vgx-runtime/objects",
                     "vita-vgx-game/eboot.bin", "case-shim/nested"):
            with self.subTest(path=name), self.build_fixture() as fixture:
                root, sdk, deps, build, verify = fixture
                target = sdk / "keep"
                target.write_bytes(b"abc")
                link = build / name
                link.parent.mkdir(exist_ok=True)
                if link.exists():
                    link.unlink()
                link.symlink_to(target)
                with self.assertRaisesRegex(ValueError, "symlink"):
                    verify()
                self.assertEqual(target.read_bytes(), b"abc")

    def test_build_directory_cannot_be_an_existing_source_subtree(self):
        with self.build_fixture() as fixture:
            root, sdk, deps, build, verify = fixture
            with self.assertRaisesRegex(ValueError, "source tree"):
                verify(root / "decomp")
            self.assertFalse((root / "decomp").exists())

    def test_normal_outputs_and_source_header_aliases_verify(self):
        with self.build_fixture() as fixture:
            root, sdk, deps, build, verify = fixture
            header = root / "Example.h"
            header.write_text("#pragma once\n")
            shim = build / "case-shim"
            shim.mkdir()
            (shim / "example.h").symlink_to(header)
            verify()

    def test_empty_runtime_inventory_cannot_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "inventory.tsv").write_text("source\tselection\tgroup\treason\n")
            result = self.run_tool("vita_runtime_report_vgx.py", ROOT, out)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((out / "libgalaxy_vita_runtime_vgx.a").exists())

    def test_hash_gate_rejects_changed_or_missing_input(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "input.a"
            file.write_bytes(b"abc")
            records = {"input.a": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}
            builder.verify_files(root, records, "test input")
            file.write_bytes(b"abd")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                builder.verify_files(root, records, "test input")
            file.unlink()
            with self.assertRaisesRegex(ValueError, "missing"):
                builder.verify_files(root, records, "test input")

    def test_player_build_environment_ignores_experiment_flags(self):
        builder = self.load_builder()
        env = builder.build_environment(Path("/sdk"), Path("/build"), {
            "PATH": "/usr/bin", "CPATH": "/unreviewed", "CXXFLAGS": "-O0",
            "VITA_EXTRA_CFLAGS": "-DPORT_LOGGING=1", "VITA_VGX_CFLAGS": "-ffast-math",
            "VITA_MEM1_MIB": "128", "VITA_MEM2_MIB": "256",
        })
        self.assertEqual(env["CPATH"], "/build/case-shim")
        self.assertEqual(env["VITA_EXTRA_CFLAGS"], "-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1")  # (the basic log)
        self.assertEqual(env["VITA_VGX_CFLAGS"], "-O3")
        for key in ("CXXFLAGS", "VITA_MEM1_MIB", "VITA_MEM2_MIB"):
            self.assertNotIn(key, env)

    def test_logging_build_keeps_the_log_switches_out(self):
        builder = self.load_builder()
        env = builder.build_environment(Path("/sdk"), Path("/build"), {"PATH": "/usr/bin"}, logging=True)
        self.assertNotIn("VITA_EXTRA_CFLAGS", env)

    def test_recorded_hash_mismatch_fails_verify(self):
        with self.build_fixture() as fixture:
            root, sdk, deps, build, verify = fixture
            (build / "vita-vgx-game/eboot.bin").write_bytes(b"abd")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                verify()

    def test_pending_hashes_only_report(self):
        with self.build_fixture() as fixture:
            root, sdk, deps, build, verify = fixture
            (root / "docs/BUILD-VERIFICATION.json").write_text(json.dumps({"sha256": {"runtime": "PENDING"}}))
            (build / "vita-vgx-game/eboot.bin").write_bytes(b"anything")
            verify()

    def test_lto_rules_reuse_the_source_commands(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "lto"
            database = [
                {"directory": "/b", "file": "/s/decomp/src/a.cpp",
                 "command": "clang++ -O2 -MD -MT x -MF x.d -c /s/decomp/src/a.cpp -o a.o"},
                {"directory": "/b", "file": "/s/platform/b.cpp", "command": "clang++ -c b.cpp -o b.o"},
            ]
            self.assertEqual(builder.write_lto_ninja(database, out), 1)
            ninja = (out / "build.ninja").read_text()
            self.assertIn("-flto=thin", ninja)
            self.assertNotIn("x.d", ninja)
            self.assertNotIn("platform/b.cpp", ninja)
            self.assertEqual(len((out / "objects.txt").read_text().split()), 1)
            with self.assertRaisesRegex(ValueError, "No game units"):
                builder.write_lto_ninja(database[1:], Path(directory) / "none")

    def test_inputs_list_the_link_set(self):
        inputs = json.loads((TOOLS / "inputs.json").read_text())
        self.assertIn("lib/libvitaGL.a", inputs["dependencies"])
        self.assertIn("share/vita.toolchain.cmake", inputs["sdk"])


if __name__ == "__main__":
    unittest.main()

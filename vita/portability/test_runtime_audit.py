import struct
import os
import tempfile
import unittest
from pathlib import Path

try:
    import runtime_audit as audit
except ModuleNotFoundError:
    audit = None


class RuntimeAuditTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(audit, "runtime audit harness is not implemented")

    def test_inventory_attempts_desktop_and_new_gx_units(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ["platform/src/os/os_mem.cpp", "platform/src/gx/draw_spans.cpp",
                     "platform/src/xr/xr_app.cpp", "platform/android/launcher.c",
                     "platform/desktop/main.cpp", "vita/runtime/input.cpp"]
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            rows = {item["source"]: item for item in audit.inventory(root)}
            self.assertEqual(rows[names[0]]["selection"], "attempt")
            self.assertEqual(rows[names[1]]["selection"], "attempt")
            self.assertEqual(rows[names[2]]["selection"], "excluded")
            self.assertEqual(rows[names[3]]["selection"], "excluded")
            self.assertEqual(rows[names[4]]["selection"], "attempt")
            self.assertEqual(rows[names[5]]["enum_abi"], "SDK-short")

    def test_generated_flags_keep_semantics_and_remove_old_outputs(self):
        entry = {"arguments": ["clang++", "--target=arm-vita-eabi", "-fno-short-enums",
                               "-include", "/source/vita/include/vita_source_prelude.h",
                               "-include", "/source/platform/include/port/compat.h",
                               "-MD", "-MF", "old.d", "-o", "old.obj", "-c", "old.cpp"]}
        flags = audit.base_command(entry)
        self.assertEqual(flags.count("-include"), 2)
        self.assertIn("-fno-short-enums", flags)
        self.assertNotIn("old.cpp", flags)
        self.assertNotIn("old.obj", flags)
        self.assertNotIn("-MD", flags)

    def test_failed_compile_never_passes_with_old_object(self):
        self.assertEqual(audit.unit_status(1, True, False), "failed")

    def test_input_drift_is_not_a_pass(self):
        self.assertEqual(audit.unit_status(0, True, True), "input-drift")

    def test_non_arm_success_is_rejected(self):
        self.assertEqual(audit.unit_status(0, False, False), "invalid-object")

    def test_elf_gate_checks_eabi_machine_and_relocatable(self):
        data = bytearray(52)
        data[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<HH", data, 16, 1, 40)
        struct.pack_into("<I", data, 36, 0x05000000)
        self.assertTrue(audit.arm_object(data))
        struct.pack_into("<H", data, 18, 62)
        self.assertFalse(audit.arm_object(data))

    def test_error_group_retains_real_message_and_locations(self):
        log = "/source/platform/src/os/os_mem.cpp:10:10: fatal error: 'sys/mman.h' file not found\n"
        rows = audit.diagnostics(log)
        self.assertEqual(rows[0]["message"], "'sys/mman.h' file not found")
        self.assertEqual(rows[0]["line"], 10)
        self.assertEqual(rows[0]["source"], "/source/platform/src/os/os_mem.cpp")

    def test_nm_archive_members_and_weak_undefined_are_distinct(self):
        rows = audit.parse_nm("game.a[unit.obj]: func T 0 4\nruntime.obj: missing U 0 0\nruntime.obj: optional w 0 0\n")
        graph = audit.symbol_graph(rows)
        self.assertIn("func", graph["definitions"])
        self.assertEqual(graph["unresolved"], {"missing": ["runtime.obj"]})
        self.assertEqual(graph["weak_unresolved"], {"optional": ["runtime.obj"]})

    def test_game_definition_resolves_runtime_reference_but_not_duplicate(self):
        rows = audit.parse_nm("game.a[a.obj]: f T 0 4\nruntime.obj: f U 0 0\nruntime.obj: g T 0 4\nother.obj: g T 0 4\n")
        graph = audit.symbol_graph(rows)
        self.assertEqual(graph["unresolved"], {})
        self.assertEqual(graph["duplicates"]["g"], ["other.obj", "runtime.obj"])

    def test_newlib_gcc_link_does_not_mix_sony_libc_unwinder(self):
        self.assertTrue(hasattr(audit, "sdk_link_names"), "audited SDK library selection is missing")
        self.assertNotIn("libSceLibc_stub.a", audit.sdk_link_names())
        self.assertIn("libpthread.a", audit.sdk_link_names())
        self.assertIn("libSceGxm_stub.a", audit.sdk_link_names(), "real GPU adapters require the GXM import library")
        self.assertIn("libSceDisplay_stub.a", audit.sdk_link_names(), "native queued scanout requires Display imports")

    def test_delta_selects_only_new_or_changed_inputs(self):
        self.assertTrue(hasattr(audit, "needs_compile"), "bounded runtime delta selection is missing")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "unit.cpp"
            header = root / "header.h"
            source.write_text("#include \"header.h\"\n")
            header.write_text("int f();\n")
            row = {"source": "unit.cpp"}
            old = {"source_sha256_after": audit.digest(source), "dependencies": {str(header): audit.digest(header)}}
            self.assertTrue(audit.needs_compile(row, None, root))
            self.assertFalse(audit.needs_compile(row, old, root))
            header.write_text("int f(int);\n")
            self.assertTrue(audit.needs_compile(row, old, root))

    def test_boundary_groups_partition_without_hiding_symbols(self):
        self.assertTrue(hasattr(audit, "boundary_groups"), "API boundary grouping is missing")
        symbols = ["sigaction", "DCFlushRange", "port_vr_wipe", "sceNetSend", "unknown_api"]
        groups = audit.boundary_groups(symbols)
        self.assertEqual(groups["scheduler_allocator_sdk_gaps"], ["sigaction"])
        self.assertEqual(groups["memory_cache"], ["DCFlushRange"])
        self.assertEqual(groups["vr_game_hooks"], ["port_vr_wipe"])
        self.assertEqual(groups["sdk_service_library_closure"], ["sceNetSend"])
        self.assertEqual(sorted(symbol for group in groups.values() for symbol in group), sorted(symbols))

    def test_optional_generated_bundle_retries_failed_and_missing_header_builds(self):
        self.assertTrue(hasattr(audit, "generated_inputs"), "optional generated shader inputs are not tracked")
        base = Path(os.environ.get("VITA_RUNTIME_TEST_TMP",
                    Path(__file__).resolve().parents[2] / "build-vita-runtime-audit-tests"))
        base.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory)
            name = "platform/src/gx/vita_shader_bundle.cpp"
            source = root / name
            source.parent.mkdir(parents=True)
            source.write_text("// optional generated include fixture\n")
            (root / "CMakeLists.txt").touch()
            row = {"source": name}
            old = {"source_sha256_after": audit.digest(source), "dependencies": {}, "status": "failed"}
            self.assertTrue(audit.needs_compile(row, old, root), "old records must acquire optional input provenance")
            old["generated_inputs"] = audit.generated_inputs(root, name)
            self.assertFalse(audit.needs_compile(row, old, root), "unchanged failure should not rerun")
            header = root / "build-gxm-material/vita_shader_bundle.h"
            header.parent.mkdir()
            header.write_text("// corrected generated include\n")
            self.assertTrue(audit.needs_compile(row, old, root), "appearance must retry a failed/no-bundle compile")
            self.assertIn("build-gxm-material/vita_shader_bundle.h", audit.snapshot(root))
            old["generated_inputs"] = audit.generated_inputs(root, name)
            old["status"] = "completed"
            self.assertFalse(audit.needs_compile(row, old, root))
            header.write_text("// a new verified bundle\n")
            self.assertTrue(audit.needs_compile(row, old, root))
            old["generated_inputs"] = audit.generated_inputs(root, name)
            header.unlink()
            self.assertTrue(audit.needs_compile(row, old, root), "removal cannot silently reuse embedded bytes")

    def test_archive_gate_accepts_new_verified_build_not_stale_or_partial_reports(self):
        self.assertTrue(hasattr(audit, "validate_game_archive"), "verified rebuild archive gate is missing")
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory) / "game.a"
            game.write_bytes(b"complete-build-fixture")
            report = {"counts": {"completed": 1834}, "ninja_exit_code": 0,
                      "inventory": {"matched": True}, "source_changes_during_build": [],
                      "archive": {"completed": True, "member_count": 1834, "sha256": audit.digest(game)}}
            self.assertEqual(audit.validate_game_archive(game, report), audit.digest(game))
            report["archive"]["completed"] = False
            with self.assertRaises(RuntimeError): audit.validate_game_archive(game, report)
            report["archive"]["completed"] = True
            report["source_changes_during_build"] = ["changed.h"]
            with self.assertRaises(RuntimeError): audit.validate_game_archive(game, report)
            report["source_changes_during_build"] = []
            game.write_bytes(b"different-archive")
            with self.assertRaises(RuntimeError): audit.validate_game_archive(game, report)


if __name__ == "__main__":
    unittest.main()

import json
from pathlib import Path
import struct
import tempfile
import unittest

from build_report import inspect_unit
from standalone_compile import base_command


class StandaloneCommandTests(unittest.TestCase):
    def test_reuses_flags_without_old_source_or_dependency_outputs(self):
        entry = {"arguments": ["clang++", "--target=arm-vita-eabi", "-fno-short-enums",
                               "-include", "compat.h", "-MD", "-MT", "Game.obj",
                               "-MF", "Game.obj.d", "-o", "Game.obj", "-c", "Game.cpp"]}
        self.assertEqual(base_command(entry), ["clang++", "--target=arm-vita-eabi",
                                               "-fno-short-enums", "-include", "compat.h"])


class InspectUnitTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "Game.cpp"
        self.source.write_text("// fixture\n")
        self.output = self.root / "Game.cpp.o"
        self.command = ["clang++", "--target=arm-vita-eabi", "-o", str(self.output), "-c", str(self.source)]
        self.entry = {"directory": str(self.root), "file": str(self.source), "arguments": self.command}

    def record(self, exit_code=0):
        import hashlib
        record = {"exit_code": exit_code, "command": self.command,
                  "source_sha256": hashlib.sha256(self.source.read_bytes()).hexdigest(),
                  "source_changed_during_compile": False, "log": str(self.output) + ".compiler.log"}
        Path(str(self.output) + ".result.json").write_text(json.dumps(record))

    def arm_object(self, machine=40):
        ident = b"\x7fELF\x01\x01\x01" + b"\0" * 9
        self.output.write_bytes(ident + struct.pack("<HHIIIIIHHHHHH", 1, machine, 1, 0, 0, 0, 0x05000000, 52, 0, 0, 0, 0, 0))

    def test_success_requires_native_arm_object_and_record(self):
        self.record()
        self.arm_object()
        self.assertEqual(inspect_unit(self.entry)["status"], "completed")

    def test_failed_compile_is_not_counted_from_an_old_object(self):
        self.arm_object()
        self.record(exit_code=1)
        self.assertEqual(inspect_unit(self.entry)["status"], "failed")

    def test_missing_record_is_unattempted(self):
        self.arm_object()
        self.assertEqual(inspect_unit(self.entry)["status"], "unattempted")

    def test_changed_source_is_stale(self):
        self.record()
        self.arm_object()
        self.source.write_text("// changed\n")
        self.assertEqual(inspect_unit(self.entry)["status"], "stale")

    def test_changed_flags_are_stale(self):
        self.record()
        self.arm_object()
        self.entry["arguments"] = self.command + ["-O3"]
        self.assertEqual(inspect_unit(self.entry)["status"], "stale")

    def test_host_object_is_invalid(self):
        self.record()
        self.arm_object(machine=62)
        self.assertEqual(inspect_unit(self.entry)["status"], "invalid_object")

    def test_ninja_dependency_output_flags_do_not_change_compile_identity(self):
        self.record()
        self.arm_object()
        path = Path(str(self.output) + ".result.json")
        record = json.loads(path.read_text())
        record["command"] += ["-MD", "-MT", str(self.output), "-MF", str(self.output) + ".d"]
        path.write_text(json.dumps(record))
        self.assertEqual(inspect_unit(self.entry)["status"], "completed")


if __name__ == "__main__":
    unittest.main()

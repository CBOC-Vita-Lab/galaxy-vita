import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import bundle_vita_shaders as bundle


class BundleTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build-gxm-shader-bundle-tests"
        scratch.mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=scratch)
        self.root = Path(self.tmp.name)
        (self.root / "build-gx-vita-shader").mkdir()
        (self.root / "build-shacc-tools").mkdir()
        self.source = self.root / "build-gx-vita-shader/vertex.cg"
        self.gxp = self.root / "build-shacc-tools/vertex.gxp"
        self.report = self.root / "build-shacc-tools/vertex.json"
        self.source.write_bytes(b"float4 main(float4 p){return p;}\n")
        self.gxp.write_bytes(b"GXP fixture bytes")
        self.data = dict(accepted=True, source=str(self.source), output=str(self.gxp), stage="vertex", entry="main",
            fastmath=False, fastprecision=False, fastint=False, optimization=3, gxp_size=17,
            source_sha256=hashlib.sha256(self.source.read_bytes()).hexdigest(),
            gxp_sha256=hashlib.sha256(self.gxp.read_bytes()).hexdigest())
        self.save()

    def tearDown(self):
        self.tmp.cleanup()

    def save(self):
        self.report.write_text(json.dumps(self.data))

    def test_verified_source_and_blob_records_retain_original_bytes(self):
        entries = bundle.read_reports(self.root, [self.report])
        self.assertEqual(entries[0][1], self.source.read_bytes().decode("ascii"))
        self.assertEqual(entries[0][2], b"GXP fixture bytes")

    def test_source_and_blob_drift_both_reject(self):
        self.source.write_bytes(b"changed shader source")
        with self.assertRaises(ValueError): bundle.read_reports(self.root, [self.report])
        self.data["source_sha256"] = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.save(); self.gxp.write_bytes(b"changed binary")
        with self.assertRaises(ValueError): bundle.read_reports(self.root, [self.report])

    def test_incomplete_or_fast_compiler_records_reject(self):
        for field, value in (("accepted", False), ("entry", "other"), ("stage", "geometry"),
                ("fastmath", True), ("fastprecision", None), ("fastint", 0),
                ("optimization", True), ("gxp_size", 8), ("runner_error", "timeout")):
            original = dict(self.data); self.data[field] = value; self.save()
            with self.subTest(field=field):
                with self.assertRaises(ValueError): bundle.read_reports(self.root, [self.report])
            self.data = original

    def test_paths_cannot_escape_owned_shader_builds(self):
        outside = self.root / "private.gxp"; outside.write_bytes(self.gxp.read_bytes())
        self.data["output"] = str(outside); self.save()
        with self.assertRaises(ValueError): bundle.read_reports(self.root, [self.report])

    def test_redirected_build_directories_reject_before_external_read_or_write(self):
        checkout = self.root / "checkout"
        checkout.mkdir()
        (self.root / "build-gxm-material").mkdir()
        for prefix in ("build-shacc-tools", "build-gx-vita-shader", "build-gxm-material"):
            (checkout / prefix).symlink_to(self.root / prefix, target_is_directory=True)
        for prefix, name in (("build-shacc-tools", "vertex.json"), ("build-shacc-tools", "vertex.gxp"),
                ("build-gx-vita-shader", "vertex.cg"), ("build-gxm-material", "vita_shader_bundle.h")):
            with self.subTest(prefix=prefix, name=name):
                with self.assertRaises(ValueError): bundle.owned(f"{prefix}/{name}", checkout, prefix)
        output = self.root / "build-gxm-material/vita_shader_bundle.h"
        with mock.patch.object(bundle, "__file__", str(checkout / "tools/bundle_vita_shaders.py")), \
                mock.patch.object(sys, "argv", ["bundle_vita_shaders.py", "--report", str(self.report)]):
            with self.assertRaises(ValueError): bundle.main()
        self.assertFalse(output.exists(), "ownership rejection must happen before any redirected output write")

    def test_duplicates_are_deduplicated_but_conflicting_artifacts_reject(self):
        self.assertEqual(len(bundle.read_reports(self.root, [self.report, self.report])), 1)
        other = self.root / "build-shacc-tools/other.gxp"; other.write_bytes(b"different fixture")
        self.data["output"] = str(other); self.data["gxp_sha256"] = hashlib.sha256(other.read_bytes()).hexdigest()
        self.save(); second = self.root / "build-shacc-tools/second.json"; second.write_text(self.report.read_text())
        self.data["output"] = str(self.gxp); self.data["gxp_sha256"] = hashlib.sha256(self.gxp.read_bytes()).hexdigest(); self.save()
        with self.assertRaises(ValueError): bundle.read_reports(self.root, [self.report, second])


if __name__ == "__main__":
    unittest.main()

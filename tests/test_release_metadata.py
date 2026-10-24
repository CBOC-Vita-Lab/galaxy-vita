"""The release files agree with each other: package hashes, manifests, checksums and source inventory."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PENDING = "PENDING-VPK"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_updater():
    spec = importlib.util.spec_from_file_location("update_checksums", ROOT / "tools/update-checksums.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((ROOT / "cboc-release.json").read_text())
        self.release = json.loads((ROOT / "docs/RELEASE.json").read_text())

    def test_manifests_agree(self):
        self.assertEqual(self.release["package"], self.manifest["package"])
        self.assertEqual(self.release["releaseId"], self.manifest["releaseId"])
        self.assertEqual(self.release["version"], self.manifest["version"])
        self.assertEqual(self.manifest["releaseId"], "galaxy-v" + self.manifest["version"])
        self.assertRegex(self.manifest["version"], r"^\d+\.\d+$")
        self.assertEqual(self.manifest["titleId"], "SMGV00001")
        self.assertEqual(self.release["titleId"], "SMGV00001")
        self.assertEqual(self.manifest["package"]["path"], "Galaxy-Vita.vpk")

    def test_installer_reads_the_galaxy_inputs(self):
        kinds = {item["kind"] for item in self.manifest["requiredInputs"]}
        self.assertEqual(kinds, {"galaxy-disc", "shader-compiler"})
        self.assertEqual(self.manifest["destinations"]["data"], "ux0:data/smg/game")
        self.assertEqual(self.manifest["releaseState"], "ready")
        for path in ("ux0:data/smg/saves", "ux0:data/smg/settings.ini"):
            self.assertIn(path, self.manifest["protectedPaths"])
        for digest in self.manifest["appUpdateFrom"]:
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_package_matches_manifest(self):
        package = ROOT / self.manifest["package"]["path"]
        if not package.is_file():
            self.assertEqual(self.manifest["package"]["sha256"], PENDING)
            self.skipTest("Galaxy-Vita.vpk is not in place yet")
        self.assertEqual(sha(package.read_bytes()), self.manifest["package"]["sha256"])
        with zipfile.ZipFile(package) as vpk:
            self.assertIsNone(vpk.testzip())
            names = set(vpk.namelist())
            self.assertEqual(sha(vpk.read("eboot.bin")), self.manifest["package"]["runtimeSha256"])
            self.assertIn("sce_sys/param.sfo", names)
            self.assertNotIn("sce_sys/loading-title.rgba", names)   # a capture of the game: never shipped
            self.assertFalse(any(n.endswith(("hdtex.pack", "libshacccg.suprx", ".thp", ".arc")) for n in names))

    def test_checksums_cover_the_repository(self):
        listed = {}
        for line in (ROOT / "SHA256SUMS").read_text().splitlines():
            digest, path = line.split("  ", 1)
            listed[path] = digest
        self.assertEqual(set(listed), set(load_updater().repository_files()))
        for path, digest in listed.items():
            with self.subTest(path=path):
                self.assertEqual(sha((ROOT / path).read_bytes()), digest)

    def test_source_manifest_matches_tree(self):
        inventory = json.loads((ROOT / "docs/SOURCE-MANIFEST.json").read_text())
        self.assertGreater(len(inventory["files"]), 4000)
        for item in inventory["files"]:
            with self.subTest(path=item["path"]):
                self.assertEqual(sha((ROOT / item["path"]).read_bytes()), item["sha256"])

    def test_no_game_data_or_private_files(self):
        updater = load_updater()
        banned = re.compile(r"\.(iso|rvz|wbfs|thp|arc|rarc|szs|brstm|pack|suprx)$|(^|/)(saves|cooked|prepared-game)/", re.I)
        for path in updater.repository_files():
            with self.subTest(path=path):
                self.assertIsNone(banned.search(path))

    def test_moved_pages_are_linked_where_they_now_live(self):
        pages = [*ROOT.glob("*.md"), *(ROOT / "docs").glob("*.md"), *(ROOT / "tools").glob("*/README.md")]
        for page in pages:
            text = page.read_text()
            for target in re.findall(r"\]\(([^)#]+)(?:#[^)]*)?\)", text):
                if target.startswith("http"):
                    continue
                with self.subTest(page=page.name, link=target):
                    if "store/screenshots/" in target:
                        continue   # the screenshot is added before publishing
                    self.assertTrue((page.parent / target).exists(), target)


if __name__ == "__main__":
    unittest.main()

"""The public package carries no game artwork: no title picture in the tree, none in the VPK recipe, no packaging guard for one,
and the loading screen and Settings read the player's own capture from ux0:data/smg/art (made by the game, port/title_art.h)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LoadingArtworkTests(unittest.TestCase):
    def test_no_title_picture_in_the_tree(self):
        self.assertFalse((ROOT / "vita/vgx-game/sce_sys/loading-title.rgba").exists())
        self.assertFalse((ROOT / "vita/vgx-game/loading_artwork.cmake").exists())
        self.assertEqual([p for p in (ROOT / "vita/vgx-game/sce_sys").rglob("*.rgba")], [])

    def test_vpk_recipe_lists_only_neutral_pictures(self):
        source = (ROOT / "vita/vgx-game/CMakeLists.txt").read_text()
        self.assertNotIn("loading-title", source)
        self.assertNotIn("loading_artwork", source)
        self.assertNotIn("smg_loading_picture", source)
        listed = set(re.findall(r'FILE "\$\{sys\}/([^"]+)"', source))
        self.assertEqual(listed, {"icon0.png", "pic0.png", "livearea/contents/bg.png", "livearea/contents/startup.png",
                                  "livearea/contents/settings.png", "livearea/contents/template.xml"})
        self.assertIn("title_art_runtime.cpp", source)

    def test_both_readers_use_the_players_capture_path(self):
        loading = (ROOT / "platform/vgx/vita/loading_screen.h").read_text()
        backdrop = (ROOT / "platform/include/port/settings_backdrop.h").read_text()
        art = (ROOT / "platform/include/port/title_art.h").read_text()
        for text in (loading, backdrop, art):
            self.assertIn('"ux0:data/smg/art/title.rgba"', text)
            self.assertNotIn("loading-title.rgba", text)

    def test_nothing_else_refers_to_the_old_picture(self):
        skip = {"SHA256SUMS", "docs/SOURCE-MANIFEST.json", "docs/SOURCE.md", "CHANGELOG.md", "tests/test_loading_artwork.py", "tests/settings_runtime.cpp",
                "tests/loading_artwork.cpp", "tests/test_title_art.py"}
        for path in list(ROOT.glob("platform/**/*")) + list(ROOT.glob("vita/**/*")) + list(ROOT.glob("tools/*.sh")) + list(ROOT.glob("docs/*.md")):
            if not path.is_file() or str(path.relative_to(ROOT)) in skip or path.suffix in {".png", ".rgba", ".inc", ".a", ".o"}:
                continue
            try:
                text = path.read_text()
            except UnicodeDecodeError:
                continue
            with self.subTest(path=str(path.relative_to(ROOT))):
                if "docs/ASSETS.md" == str(path.relative_to(ROOT)) or path.name == "make_livearea_title.py":
                    continue
                self.assertNotIn("loading-title.rgba", text)


if __name__ == "__main__":
    unittest.main()

import importlib.util
from pathlib import Path
import tempfile
import unittest

script = Path(__file__).resolve().parents[1] / "tools/c182_vita3k_smoke.py"
spec = importlib.util.spec_from_file_location("c186_retail", script)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class RetailProfile(unittest.TestCase):
    def test_retail_only(self):
        for normal in (True, False):
            with tempfile.TemporaryDirectory() as directory:
                data = Path(directory)
                meta = dict(stage="HoneyBeeKingdomGalaxy", normal_boot=normal,
                            profile="emulator / retail", game_cache_imported=False)
                env = smoke.retail_env(meta["stage"], normal)
                self.assertEqual({line for line in env.splitlines() if line.startswith("GXV_")},
                                 {"GXV_VSOUT=1", "GXV_PICTURE_MODE=60", "GXV_STDERR_FILE=1"})
                for name in ("game", "saves"):
                    (data/name).mkdir()
                if not normal:
                    (data/"input.txt").touch()
                settings = data/"env.txt"
                settings.write_text(env)
                smoke.check_retail(data, meta)
                for change in ("GXV_VSOUT=0", "GXV_SHADERPACK=1", "GXV_USERPACK=0", "GXV_HRW_WARP_DUMP=4"):
                    settings.write_text(env + change + "\n")
                    with self.assertRaises(ValueError):
                        smoke.check_retail(data, meta)
                settings.write_text(env)
                for name in ("shadercache", "settings.ini"):
                    extra = data/name
                    extra.touch()
                    with self.assertRaises(ValueError):
                        smoke.check_retail(data, meta)
                    extra.unlink()
                with self.assertRaises(ValueError):
                    smoke.check_retail(data, dict(meta, game_cache_imported=True))


if __name__ == "__main__":
    unittest.main()

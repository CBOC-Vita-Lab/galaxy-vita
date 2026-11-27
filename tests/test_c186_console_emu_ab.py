import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/c186_emu_ab"))
import compare
import replay
import witness


def stamp(seconds):
    minutes, seconds = divmod(seconds, 60)
    return f"00:{int(minutes):02d}:{seconds:06.3f}"


class EmulatorComparisonTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)

    def arm(self, label, *, windows=(6, 11, 16, 21, 26), seconds=5, turns=60,
            retry=35, reason="route-complete", rc=0, marker=True):
        folder = self.folder / label
        folder.mkdir(exist_ok=True)
        env = (replay.BASE + ("GXV_HRW=0\n" if label == "B" else "")).encode()
        (folder / "env.txt").write_bytes(env)
        (folder / "inputs.json").write_text(json.dumps({"game": "fixture", "env": hashlib.sha256(env).hexdigest()}))
        (folder / "outcome.json").write_text(json.dumps({"reason": reason, "container_exit": rc}))
        origin = 10 if label == "A" else 130  # boot timing is intentionally not shared
        rows = [f"{stamp(1)} [mp] core ready build=3.0-C186.1 proto=5", f"{stamp(origin)} {compare.ANCHOR}"]
        mode = "5 " if label == "A" else "0(the env.txt override) "
        (folder / "modekeys.txt").write_text(f"{stamp(1)} modekeys: Performance mode: GXV_HRW={mode}GXV_OTHER=1\n")
        def fps(at, duration, shown, rate, hitches=0, longest=17):
            return (f"{stamp(origin+at)} fps: {duration:.1f} s: shown {shown:.1f} fps; game {rate:.1f} turns/s; "
                    f"0 skipped; {hitches} hitches over 100 ms (longest {longest} ms); picture 960x544")
        # These two extreme outliers straddle opposite route boundaries.
        rows.append(fps(0.5, 5, 1, 1, 99, 2500))
        rows.extend(fps(t, seconds, 55 if label == "A" else 45, turns) for t in windows)
        rows.append(fps(retry-1, 5, 1, 1, 99, 2500))
        rows.append(f"{stamp(origin+retry)} {compare.LOAD} (key 17bab031): wall 1.00 s on the loading thread")
        if marker:
            rows.append(f"{stamp(origin+retry+4)} {compare.ANCHOR}")
        (folder / "guest-samples.txt").write_text("\n".join(rows) + "\n")
        return folder

    def decision(self):
        with mock.patch.object(compare, "world", return_value={"distinct": 256, "lit": 1.0, "colorful": 0.75}):
            return compare.compare(self.folder)

    def rejected(self, result):
        self.assertFalse(result["comparison_accepted"])
        self.assertTrue(result["limitations"])
        self.assertNotIn("on_minus_off_shown_fps", result)

    def test_matched_arms_drop_straddling_windows_and_use_guest_progress(self):
        self.arm("A")
        self.arm("B")
        result = self.decision()
        self.assertTrue(result["comparison_accepted"], result["limitations"])
        self.assertEqual(result["on_minus_off_shown_fps"], 10)
        for key in ("A_assist_on", "B_assist_off"):
            r = result[key]
            self.assertEqual((r["windows"], r["seconds"], r["sampled_turns_approx"]), (5, 25, 1500))
            self.assertEqual((r["hitches_gt100ms"], r["worst_hitch_ms"]), (0, 17))
            self.assertEqual((r["first_sample_start_offset_s"], r["last_sample_end_offset_s"]), (1, 26))

    def test_different_progress_or_stalled_game_withholds_delta(self):
        self.arm("A")
        for turn_rate in (35, 0):
            with self.subTest(turn_rate=turn_rate):
                self.arm("B", turns=turn_rate)
                result = self.decision()
                self.rejected(result)
                self.assertTrue(any("turns" in issue or "game progress" in issue for issue in result["limitations"]))

    def test_equal_total_turns_do_not_hide_mismatched_phase(self):
        self.arm("A")
        # Same 1500 turns in both arms; only the first, then only the last,
        # sampled boundary changes by five seconds of a 33-second route.
        for ends, dt, turns, edge in (((10, 14, 18, 22, 26), 4, 75, "first_sample_start_offset_s"),
                                     ((7, 13, 19, 25, 31), 6, 50, "last_sample_end_offset_s")):
            with self.subTest(ends=ends):
                self.arm("B", windows=ends, seconds=dt, turns=turns)
                result = self.decision()
                self.rejected(result)
                self.assertIn("sampled route phase differs at " + edge, result["limitations"])

    def test_interior_samples_must_be_contiguous_and_effective_mode_exact(self):
        self.arm("A")
        folder = self.arm("B", windows=(6, 11, 21, 26))
        result = self.decision()
        self.rejected(result)
        self.assertIn("missing, overlapping or out-of-order play windows", result["limitations"])
        self.arm("B")
        modekeys = folder / "modekeys.txt"
        modekeys.write_text(modekeys.read_text().replace("GXV_HRW=0(", "GXV_HRW=05("))
        self.assertIn("effective HRW mode not confirmed", self.decision()["limitations"])

    def test_missing_endpoint_or_failed_launch_withholds_delta(self):
        self.arm("A")
        for args in ({"marker": False}, {"reason": "timeout"}, {"rc": 3}):
            with self.subTest(args=args):
                self.arm("B", **args)
                self.rejected(self.decision())

    def test_blank_exported_framebuffer_is_not_visual_proof(self):
        public = self.arm("A")
        self.arm("B")
        # Synthesize a valid black XWD using the captured viewport dimensions;
        # exercise the actual filter-0 RGB8 PNG writer and reader without PIL.
        h = [0] * 25
        h[0:2], h[4:6], h[11:13] = [100, 7], [1280, 800], [32, 1280*4]
        (self.folder / "play.xwd").write_bytes(struct.pack(">25I", *h) + bytes(1280*800*4))
        witness.export(self.folder, public)
        self.assertEqual(compare.world(public / "play-world.png"), {"distinct": 1, "lit": 0.0, "colorful": 0.0})
        self.assertIsNone(compare.world(public / "return-world.png"))
        result = compare.compare(self.folder)
        self.rejected(result)
        self.assertTrue(any("play framebuffer" in row for row in result["limitations"]))


if __name__ == "__main__":
    unittest.main()

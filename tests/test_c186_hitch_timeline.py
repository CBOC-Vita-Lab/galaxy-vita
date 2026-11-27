import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from c186_hitch_timeline import correlate


class TimelineTests(unittest.TestCase):
    def setUp(self):
        g = "[mp] mem ui archive request Map (/Layout/Map.arc) loaded-now=1"
        h = "|I| [export_sceIoOpen]: Opening file: ux0:/data/a/Map.arc"
        self.guest = ["00:00:01.003 " + g, "00:00:25.003 " + g,
                      "00:00:30.003 hitch: frame 400 130 ms (back end 105): label",
                      "00:00:30.004 stutter: frame 400 took 105 ms in the back end (in play): sum, present 104 ms, other",
                      "00:00:32.003 hitch: frame 500 124 ms (back end 20): label"]
        self.host = ["[23:59:46.000] " + h, "[00:00:10.000] " + h,
                     "[00:00:14.915] |I| [save_pipeline_cache]: Saving pipeline cache...",
                     "[00:00:14.998] |I| [save_pipeline_cache]: Pipeline cache saved"]

    def test_midnight_and_independent_events(self):
        a, b = correlate(self.guest, self.host, [(1, 1), (2, 2)])["frames"]
        self.assertEqual((a["present_ms"], a["overlapping_writes"][0]["overlap_ms"]), (104, 83))
        self.assertEqual(a["overlapping_writes"][0]["end_before_guest_log_ms"], 2)
        self.assertIsNone(b["present_ms"])
        self.assertEqual(b["overlapping_writes"], [])

    def test_rejects_duplicate_misleading_or_unstable_anchors(self):
        for pairs in [[(1, 1)], [(1, 1), (1, 1)], [(1, 1), (3, 3)], [(1, 2), (2, 1)]]:
            with self.assertRaises(ValueError):
                correlate(self.guest, self.host, pairs)
        self.host[1] = self.host[1].replace("10.000", "10.100")
        with self.assertRaises(ValueError):
            correlate(self.guest, self.host, [(1, 1), (2, 2)])

    def test_unfinished_write_does_not_prove_overlap(self):
        result = correlate(self.guest, self.host[:-1], [(1, 1), (2, 2)])
        self.assertEqual(result["frames"][0]["overlapping_writes"], [])


if __name__ == "__main__":
    unittest.main()

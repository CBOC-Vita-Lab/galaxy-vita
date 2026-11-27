import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from c186_console_receipt import summarize


BUILD = "02:01:17.489 [mp] core ready build=3.0-C186 proto=5"
BOOT = "00:00:00.000 Super Mario Galaxy Vita, basic log: frame rate and hitches"
QUALITY = "settings: picture mode Quality (30) (GXV_PICTURE_MODE=30): GXV_FPS_LIMIT=30"
BEE = "00:41:48.163 mode: 60, stage HoneyBeeKingdomGalaxy:1, preset keys applied: GXV_FARLOGIC=3"
ASTRO = "00:42:01.000 mode: 60, stage AstroGalaxy, preset keys applied: none (GXV_STAGE_PRESETS=0)"
LEVEL = "00:41:47.035 farlogic: level 2 now (was -1), dry 0, armed 0; GXV_ALOD 0"
WINDOW = "00:41:56.991 farlogic: level 3, 10.0 s, 300 turns: a turn: 101.0 marked actors decide: 60.3 in the far state"
FRAME = ("00:42:13.971 first-use: frame 803 in play, 1 ms after a pad input, 7.8 s after the screen opened: "
         "6 ms in the back end: programs linked by draws 2 (3 ms; shader compiler 1 runs, 2 ms), render targets made "
         "1 240x136 (MaskSnap:24977), texture uploads 4 (17 KiB, 5 ms); recorded in the player list (GXV_PLAYERWARM)")
SNAPSHOT = ("00:43:06.187 first-use: stage 17bab031 periodic, 60 s after the screen opened: still paid in play: 7 frames; "
            "programs linked by draws 2 (3 ms; 1 of them within 6000 ms of a pad input = 2 ms; shader compiler 1 runs 2 ms), "
            "render targets made 4, texture uploads 9 (33 KiB, 8 ms), worst such frame 16 ms; player list: 1 links after a pad "
            "input, 1 records added this stage")
SOCKETS = "[net-https] sockets: 1 open (1 epoll), 21 opened and 20 closed so far, most at once 1, refused 0"
HTTPS = "[net-https] POST private-peer.invalid:443 -> OK status=200 tls=1.3 reused 25100ms 27B"


class ConsoleReceiptTests(unittest.TestCase):
    def test_stage_context_and_eligible_actor_are_not_global(self):
        log = [LEVEL, BEE, BEE, ASTRO, WINDOW, "far-pose-test: restored first level-3 half-rate pose: Tico"]
        run, = summarize(log)
        self.assertEqual(len(run["stage_changes"]), 2)
        before, after = run["far_levels"]
        self.assertEqual((before["level"], before["last_stage_context"]), (2, None))
        self.assertEqual((after["level"], after["last_stage_context"]["stage"]), (3, "AstroGalaxy"))
        self.assertEqual(run["restored_pose_markers"], [
            {"line": 6, "last_stage_context": {"stage": "AstroGalaxy", "scenario": None}, "model": "Tico"}
        ])
        self.assertEqual(summarize([BEE])[0]["far_levels"], [])  # a preset is not a live level

    def test_separate_raw_and_cumulative_after_stage_transition(self):
        run, = summarize([BEE, FRAME, ASTRO, SNAPSHOT, SNAPSHOT.replace("periodic", "left")])
        frame, = run["first_use_frames"]
        self.assertEqual((frame["frame"], frame["draw_links"], frame["compiler_runs"], frame["targets"], frame["uploads"]),
                         (803, 2, 1, 1, 4))
        self.assertEqual((frame["input_age_ms"], frame["no_input_window_ms"]), (1, None))
        snapshots = run["first_use_stage_snapshots"]
        self.assertEqual([(s["stage"], s["reason"], s["frames"]) for s in snapshots],
                         [("HoneyBeeKingdomGalaxy", "periodic", 7), ("HoneyBeeKingdomGalaxy", "left", 7)])
        self.assertEqual([s["upload_kib"] for s in snapshots], [33, 33])
        self.assertEqual(run["unparsed_first_use_rows"], 0)
        neither = FRAME.replace("1 ms after a pad input", "no pad input in the last 6000 ms")
        run, = summarize([neither, FRAME[:80], SNAPSHOT[:80], "first-use: stage 17bab031: player-action list: ..."])
        self.assertEqual(run["unparsed_first_use_rows"], 2)
        self.assertIsNone(run["first_use_frames"][0]["input_age_ms"])
        self.assertEqual(run["first_use_frames"][0]["no_input_window_ms"], 6000)

    def test_restart_boundaries_and_missing_markers(self):
        returned = "settings: restart returned 0x00000000; saved choices remain next-launch only"
        runs = summarize([BOOT, BUILD, BEE, "[mp] settings: restart asked (pause)", "[mp] settings: restart now",
                          returned, BOOT, QUALITY, BUILD.replace("C186", "C186.1"), LEVEL])
        self.assertEqual(len(runs), 2)
        self.assertEqual([r["event"] for r in runs[0]["restart_events"]], ["asked_from_pause", "attempted", "relaunch_call_returned"])
        self.assertEqual(runs[0]["restart_events"][-1]["return_code_u32"], 0)
        self.assertEqual(runs[1]["picture_modes"][0]["selected"], "Quality")
        self.assertEqual([s["build"] for s in runs], ["3.0-C186", "3.0-C186.1"])
        self.assertEqual(summarize([BUILD.replace("C186", "C186.2")])[0]["build"], "3.0-C186.2")
        self.assertEqual(runs[1]["protocol"], 5)
        self.assertIsNone(runs[1]["far_levels"][0]["last_stage_context"])
        self.assertEqual(runs[1]["restart_events"], [])
        self.assertEqual(summarize([returned.replace("00000000", "80000001")])[0]["restart_events"],
                         [{"line": 1, "event": "relaunch_call_returned", "return_code_u32": 0x80000001}])
        with self.assertRaisesRegex(ValueError, "separate launch logs"):
            summarize([BUILD, BUILD])
        self.assertIsNone(summarize([])[0]["build"])

    def test_net_and_private_fields(self):
        trouble = HTTPS.replace("OK status=200", "TIMEOUT (no data for too long) status=0")
        log = [HTTPS, SOCKETS, trouble, "[mp] room nickname=no data for too long",
               "far-pose-test: restored first level-3 half-rate pose: private-name",
               BEE.replace("HoneyBeeKingdomGalaxy", "private-name"), BUILD.replace("3.0-C186", "private-hash")]
        run, = summarize(log)
        self.assertEqual(run["https_requests"], [
            {"line": 1, "status": 200, "elapsed_ms": 25100, "transport_ok": True, "no_data_timeout": False},
            {"line": 3, "status": 0, "elapsed_ms": 25100, "transport_ok": False, "no_data_timeout": True},
        ])
        header_only, = summarize([HTTPS.replace("OK status=200", "TIMEOUT (total time limit) status=200")])
        self.assertFalse(header_only["https_requests"][0]["transport_ok"])
        self.assertEqual(run["socket_snapshots"][0], {"line": 2, "open": 1, "epoll": 1, "opened": 21, "closed": 20,
                                                      "peak": 1, "refused": 0})
        self.assertEqual(run["build"], "other")
        self.assertNotIn("private-", json.dumps(run))

    def test_cli_does_not_echo_paths_or_guess_source_or_relaunch(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = (Path(d) / name for name in ("private-previous", "private-present"))
            a.write_text(BOOT + "\n" + BUILD + "\n[mp] settings: restart now\n")
            b.write_text(BOOT + "\n" + BUILD + "\n" + QUALITY)
            command = [sys.executable, str(TOOLS / "c186_console_receipt.py"), str(a), str(b)]
            proc = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertNotIn("private-", proc.stdout)
            data = json.loads(proc.stdout)
            self.assertEqual(data["operator_declared_platform"], "unknown")
            self.assertEqual([len(f["sessions"]) for f in data["inputs"]], [1, 1])
            self.assertEqual(json.loads(subprocess.check_output([*command, "--platform", "emulator"]))
                             ["operator_declared_platform"], "emulator")
            a.unlink()
            proc = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)
            self.assertNotIn("private-", proc.stderr)


if __name__ == "__main__":
    unittest.main()

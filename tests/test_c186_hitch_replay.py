"""Guest control boundaries, not a renderer's 'in play' bit, delimit the replay."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "hitch_replay"))
from analyze_replay import ANCHOR, DRIVER, EMU_RELEASE, FINISHED, LOADED, MOVING_FINISHED, OPENED, match_counts, scan


def fps(t, longest):
    return (f"00:00:{t:06.3f} fps: 5.0 s: shown 60.0 fps; game 60.0 turns/s; 0 skipped; "
            f"{int(longest > 100)} hitches over 100 ms (longest {longest} ms); picture 960x544")


class Replay(unittest.TestCase):
    def test_nonoverlapping_phases_and_unknown_host_offset(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            data = root / "portable/fs/ux0/data/smg"
            data.mkdir(parents=True)
            def event(t, s): return f"00:00:{t:06.3f} {s}"
            lines = [event(5, ANCHOR), fps(5.5, 150), fps(11, 60),
                     event(14, "vgx: scene destroyed: test"), fps(20, 175),
                     event(22, ANCHOR), fps(28, 38)]
            (data / "log.txt").write_text("\n".join(lines))
            (root / "host.log").write_text(EMU_RELEASE+"\n[00:01:00.500] [save_pipeline_cache]: Saving pipeline cache...")
            (root/"start.json").write_text("{}")
            (root/"outcome.json").write_text(json.dumps({"exit":0,"stop":FINISHED}))
            (root / DRIVER).parent.mkdir(parents=True)
            (root / DRIVER).write_bytes(b"cache")
            result = scan(root)
            self.assertEqual(result["coverage"]["first controlled"]["over100"], 0)
            self.assertEqual(result["coverage"]["second controlled"]["longest_ms"], 38)
            self.assertEqual(result["coverage"]["boundary (excluded)"]["longest_ms"], 150)
            self.assertEqual(result["coverage"]["forced re-entry / opening"]["longest_ms"], 175)
            self.assertFalse(result["independent_sync_valid"])
            self.assertIsNone(result["host_saves_guest_clock_s"])
            self.assertEqual(result["host_save_messages"], {"began": 1, "completed": 0})
            for outcome in ({"exit":-9,"stop":FINISHED},{"exit":0,"stop":"time limit"}):
                (root/"outcome.json").write_text(json.dumps(outcome))
                with self.assertRaisesRegex(ValueError,"completed supervised"):
                    scan(root)
            (root/"outcome.json").write_text(json.dumps({"exit":-15,"stop":FINISHED}))
            (data / "log.txt").write_text("\n".join(lines[:-2]))
            with self.assertRaisesRegex(ValueError, "two controllable"):
                scan(root)

    def test_wrap_both_clocks_and_reject_malformed_save_order(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            data=root/"portable/fs/ux0/data/smg"
            data.mkdir(parents=True)
            (root/"start.json").write_text("{}")
            (root/"outcome.json").write_text(json.dumps({"exit":0,"stop":FINISHED}))
            (root/DRIVER).parent.mkdir(parents=True)
            (root/DRIVER).write_bytes(b"cache")
            (data/"log.txt").write_text("\n".join([f"23:59:44.000 {LOADED}",f"23:59:45.000 {ANCHOR}",
                "23:59:58.999 vgx: scene destroyed: test",f"00:00:00.000 {LOADED}",f"00:00:01.000 {ANCHOR}"]))
            begin="[save_pipeline_cache]: Saving pipeline cache..."
            end="[save_pipeline_cache]: Pipeline cache saved"
            def host(msgs):
                lines=[EMU_RELEASE,f"[23:59:45.000] {OPENED}"]
                lines.extend("[{}] {}".format(t,msg) for t,msg in msgs)
                lines.extend([f"[00:00:01.000] {OPENED}",
                              "[00:00:02.345] [checkpoint]: Pipeline checkpoint snapshot: 14 ms, 55 driver bytes, final false",
                              "[00:00:02.346] [checkpoint]: Pipeline checkpoint disk write: 1 ms, success true"])
                (root/"host.log").write_text("\n".join(lines))
                return scan(root)
            ok=host([("23:59:59.987",begin),("00:00:00.013",end)])
            self.assertTrue(ok["independent_sync_valid"])
            self.assertEqual(ok["host_saves_guest_clock_s"][0]["duration_ms"],26)
            self.assertEqual(ok["host_checkpoint_messages"][1]["guest_t"],86401.346)
            for msgs in ([(begin,2),(begin,3),(end,4),(end,5)],[(end,2),(begin,3)]):
                bad=host([(f"23:59:5{sec}.000",msg) for msg,sec in msgs])
                self.assertIsNone(bad["host_saves_guest_clock_s"])

    def test_moving_multiple_reload_boundaries_and_short_reload_not_clean(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            data=root/"portable/fs/ux0/data/smg"
            data.mkdir(parents=True)
            (root/"start.json").write_text(json.dumps({"route":"moving"}))
            (root/"outcome.json").write_text(json.dumps({"exit":0,"stop":MOVING_FINISHED}))
            (root/DRIVER).parent.mkdir(parents=True)
            (root/DRIVER).write_bytes(b"cache")
            (root/"host.log").write_text(EMU_RELEASE)
            (data/"log.txt").write_text("\n".join([
                f"00:00:01.000 {ANCHOR}",fps(8,40),"00:00:10.000 vgx: scene destroyed: test",
                f"00:00:11.000 {ANCHOR}",fps(13,150),fps(17,40),"00:00:19.000 vgx: scene destroyed: test",
                f"00:00:20.000 {ANCHOR}",fps(22,120),fps(28,45)]))
            result=scan(root)
            self.assertEqual(result["coverage"]["moving controlled"]["windows"],3)
            self.assertEqual(result["coverage"]["boundary (excluded)"]["over100"],2)
            self.assertEqual(result["scene_destructions"],[10,19])

    def test_moving_missing_late_host_markers_need_unique_prefix(self):
        self.assertTrue(match_counts([(1,0),(2,48),(3,63)],[(10,5),(20,53)],True))
        self.assertFalse(match_counts([(1,0),(2,48),(3,96)],[(10,5),(20,53)],True))
        self.assertFalse(match_counts([(1,0),(2,48),(3,63)],[(10,5),(20,53)],False))


if __name__ == "__main__":
    unittest.main()

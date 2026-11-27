#!/usr/bin/env python3
"""Self-test of the MP test harness (tools/mp/harness) with a fake game process (harness_fakegame.py): no game build needed.

  python3 tests/mp/harness_test.py            (about 30 s; runs in a temporary folder)

Checks: run_pair.sh starts two processes together and merges their logs (game.log + stderr-only lines, no duplicates);
assert_log.py's rules pass on a good run and fail on: a guest that never follows a MOVE, a crash (symbolized crash.txt),
a write to the single-player save (save guard), an absent line, a count bound, a bad rule (exit 2); soft rules only warn;
run_scenario.py skips a scenario whose required strings the binary lacks.
"""
import json, os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
FAKE = os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py')
OUT = tempfile.mkdtemp(prefix='harness_test_')
ENV = dict(os.environ, HARNESS_BIN=FAKE, HARNESS_OUT=OUT, HARNESS_MIN_LINES='0')   # (the fake game logs a handful of lines)

HOST = ('0.3:[mp] host: room open mode=together;0.8:[mp] join: slot=1 name=guest accepted;'
        '1.0:[mp] flow move serial=1 galaxy=EggStarGalaxy scenario=1 type=0;2.0:[mp] flow move serial=2 galaxy=AstroGalaxy scenario=1 type=3')
GUEST = ('0.6:![mp] join: welcome slot=1 session=77;1.3:[mp] flow move serial=1 galaxy=EggStarGalaxy scenario=1 type=0;'
         '2.4:[mp] flow move serial=2 galaxy=AstroGalaxy scenario=1 type=3')


def pair(name, host_env, guest_env, seconds=3):
    cmd = [os.path.join(H, 'run_pair.sh'), '-n', name, '-o', OUT, '-s', str(seconds)]
    for kv in host_env:
        cmd += ['-P', 'host=' + kv]
    for kv in guest_env:
        cmd += ['-P', 'guest=' + kv]
    rc = subprocess.call(cmd, env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return rc, os.path.join(OUT, name)


def judge(folder, *rules):
    cmd = [sys.executable, os.path.join(H, 'assert_log.py'), folder, '-q']
    for r in rules:
        cmd += ['-e', r]
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


ORDER = 'order host /flow move serial=(\\d+)/ before guest /flow move serial=$1 / within 2s each'


class Harness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.good_rc, cls.good = pair('good', ['FAKE_SCRIPT=' + HOST], ['FAKE_SCRIPT=' + GUEST])

    def test_good_run_passes(self):
        self.assertEqual(self.good_rc, 0)
        rc, out = judge(self.good, 'expect host /room open/ within 2s',
                        'expect guest /join: welcome slot=1/ within 2s after host /room open/', ORDER,
                        'absent * /crash|Assert|FATAL/', 'count host /flow move/ == 2', 'save_guard *', 'exit * == 0',
                        'nocrash *', 'shots * >= 3', 'metric * wall_s < 30')
        self.assertEqual(rc, 0, out)

    def test_no_process_reaches_the_live_service_by_default(self):
        # the build's default service is the production address: the harness sets SMG_ONLINE_URL=none for every process unless the scenario names one
        import glob
        envs = sorted(glob.glob(os.path.join(self.good, '**', 'env.txt'), recursive=True))
        self.assertGreaterEqual(len(envs), 2)
        for f in envs:
            lines = open(f).read().splitlines()
            self.assertEqual([l for l in lines if l.startswith('SMG_ONLINE_URL=')], ['SMG_ONLINE_URL=none'], f)
        rc, folder = pair('own_service', ['SMG_ONLINE_URL=http://127.0.0.1:18795', 'FAKE_SCRIPT=' + HOST], ['FAKE_SCRIPT=' + GUEST])
        got = {}
        for f in glob.glob(os.path.join(folder, '**', 'env.txt'), recursive=True):
            got[f] = [l for l in open(f).read().splitlines() if l.startswith('SMG_ONLINE_URL=')]
        self.assertEqual(sorted(sum(got.values(), [])), ['SMG_ONLINE_URL=http://127.0.0.1:18795', 'SMG_ONLINE_URL=none'], got)

    def test_an_exported_service_address_is_passed_through(self):
        # a wrapper script that exports SMG_ONLINE_URL (friends_shots / door_shots) must reach the game: the default is not emitted over it
        import glob
        def online_in_logs(folder):
            return sorted(set(__import__('re').findall(r'online=(\S*)', ''.join(open(f, errors='replace').read() for f in glob.glob(os.path.join(folder, '**', 'game.log'), recursive=True)))))
        self.assertEqual(online_in_logs(self.good), ['none'])   # default: the harness switches online off
        env2 = dict(ENV, SMG_ONLINE_URL='http://127.0.0.1:18797')
        cmd = [os.path.join(H, 'run_pair.sh'), '-n', 'exported', '-o', OUT, '-s', '3', '-P', 'host=FAKE_SCRIPT=' + HOST, '-P', 'guest=FAKE_SCRIPT=' + GUEST]
        subprocess.call(cmd, env=env2, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        folder = os.path.join(OUT, 'exported')
        self.assertEqual(online_in_logs(folder), ['http://127.0.0.1:18797'])
        for f in glob.glob(os.path.join(folder, '**', 'env.txt'), recursive=True):
            self.assertNotIn('SMG_ONLINE_URL=none', open(f).read(), f)

    # ---- harnessfix (11 Nov): the judge's value comparison, tolerance, shutdown window, game-time units, starvation verdict
    def test_same_compares_values_between_consoles(self):
        rc, out = judge(self.good, 'same host,guest /flow move serial=(\\d+) galaxy=(\\w+)/ each')
        self.assertEqual(rc, 0, out)
        rc, out = judge(self.good, 'same host,guest /flow move serial=(\\d+) galaxy=(\\w+)/ first', 'same host,guest /(room open|join: welcome) mode=(\\w+)/')
        self.assertEqual(rc, 1, out)   # the host never logs the guest's welcome line: nothing to compare is a failure, not a pass
        rc, out = judge(self.good, 'same host,guest /(room open|join: welcome slot=(\\d))/ first')
        self.assertEqual(rc, 1, out)   # different captured values (None vs 1)

    def test_order_tolerance(self):
        tight = 'order guest /flow move serial=(\\d+)/ before host /flow move serial=$1 / within 5s'
        rc, out = judge(self.good, tight)   # the host logs it BEFORE the guest: "guest before host" fails without tolerance
        self.assertEqual(rc, 1, out)
        rc, out = judge(self.good, tight + ' tol 3s')
        self.assertEqual(rc, 0, out)

    def test_shutdown_window(self):
        # the process that stops first sends BYE before its own last line: a rule "no session ended before the host's end" must not trip on it
        rc, out = judge(self.good, 'absent guest /flow move/ before shutdown')
        self.assertEqual(rc, 1, out)    # real events well before the end still count
        rc, out = judge(self.good, 'absent * /never printed/ before shutdown')
        self.assertEqual(rc, 0, out)

    def test_game_time_units(self):
        rc, out = judge(self.good, 'expect host /room open/ within 2s', 'expect host /room open/ within 2ws')
        self.assertEqual(rc, 0, out)
        rc, out = judge(self.good, 'expect host /never printed/ within 1s')
        self.assertEqual(rc, 1, out)

    def test_invalid_verdict_when_starved(self):
        # fabricate frame stamps: 300 game frames over 20 wall seconds = 0.25 x real time; a failed rule then makes the run INVALID, not FAIL
        import shutil
        d = os.path.join(OUT, 'starved'); shutil.copytree(self.good, d)
        for proc in ('host', 'guest'):
            r = json.load(open(os.path.join(d, proc, 'result.json')))
            r['start_ms'] = 1_700_000_000_000; r['end_ms'] = r['start_ms'] + 40000
            json.dump(r, open(os.path.join(d, proc, 'result.json'), 'w'))
            open(os.path.join(d, proc, 'game.log'), 'w').write('')
            with open(os.path.join(d, proc, 'console.log'), 'w') as f:
                for k in range(1, 5):
                    f.write(f'{r["start_ms"] + k * 10000} gx: frame {k * 200} presented (xfb 0x0, black), 0 draws so far\n')
        rc, out = judge(d, 'expect host /never printed/ within 5ws')   # a wall-time rule: the machine may be why it failed
        self.assertEqual(rc, 3, out)
        self.assertIn('INVALID', out)
        rc, out = judge(d, 'expect host /never printed/ within 5gs')   # every rule kind: INVALID on a starved run
        self.assertEqual(rc, 3, out)
        rc, out = judge(d, 'expect host /never printed/')
        self.assertEqual(rc, 3, out)
        rc, out = judge(d, 'absent host /never printed/')
        self.assertEqual(rc, 0, out)   # a pass stays a pass

    def test_timeline_merges_without_duplicates(self):
        with open(os.path.join(self.good, 'timeline.txt')) as f:
            lines = f.read().splitlines()
        moves = [l for l in lines if 'flow move serial=1' in l]
        self.assertEqual(len(moves), 2, lines)          # once per process although each line is in game.log and stderr
        self.assertTrue(any('guest' in l and 'welcome' in l for l in lines))   # a stderr-only line is kept
        times = [float(l.split()[0]) for l in lines]
        self.assertEqual(times, sorted(times))
        with open(os.path.join(self.good, 'pair.json')) as f:
            self.assertIn('wall_s', json.load(f))

    def test_guest_not_following_fails(self):
        rc, folder = pair('nofollow', ['FAKE_SCRIPT=' + HOST], ['FAKE_SCRIPT=' + GUEST.split(';2.4')[0]])
        rc, out = judge(folder, ORDER)
        self.assertEqual(rc, 1, out)
        self.assertIn('flow move serial=2', out)

    def test_crash_is_caught_and_symbolized(self):
        rc, folder = pair('crash', ['FAKE_SCRIPT=' + HOST, 'FAKE_CRASH=1.5'], ['FAKE_SCRIPT=' + GUEST])
        self.assertNotEqual(rc, 0)
        rc, out = judge(folder, 'nocrash *')
        self.assertEqual(rc, 1, out)
        self.assertIn('desktop crash signal', out)
        self.assertTrue(os.path.exists(os.path.join(folder, 'host', 'crash.txt')))
        rc, out = judge(folder, 'exit host == 0')
        self.assertEqual(rc, 1, out)

    def test_save_write_fails_guard(self):
        rc, folder = pair('savewrite', ['FAKE_SCRIPT=' + HOST], ['FAKE_SCRIPT=' + GUEST, 'FAKE_SAVE_WRITE=1'])
        self.assertNotEqual(rc, 0)
        rc, out = judge(folder, 'save_guard host')
        self.assertEqual(rc, 0, out)
        rc, out = judge(folder, 'save_guard *')
        self.assertEqual(rc, 1, out)
        self.assertIn('CHANGED', out)
        # the original test save is never written
        with open(os.path.join(folder, 'guest', 'save-before.json')) as f:
            g = json.load(f)
        self.assertTrue(g['files'])

    def test_save_setup_and_save_files(self):
        # scenarios-final: HARNESS_SAVE_SETUP edits only the run's own copy of the save folder; save_files judges the folder list after the run
        import hashlib
        src = os.path.join(os.environ.get('HARNESS_SAVES0', '/data/saves0/saves'), 'title/00010000/524d4750/data/GameData.bin')
        before = hashlib.sha256(open(src, 'rb').read()).hexdigest()
        rc, folder = pair('savesetup', ['FAKE_SCRIPT=' + HOST, 'HARNESS_SAVE_SETUP=mpbak,rm'], ['FAKE_SCRIPT=' + GUEST, 'HARNESS_SAVE_SETUP=mptmp,tear'])
        for rule, want in (('save_files host absent GameData.bin', 0), ('save_files host present GameData.bin.mpbak', 0),
                           ('save_files host present GameData.bin', 1), ('save_files host absent GameData.bin.mpbak', 1),
                           ('save_files guest present GameData.bin GameData.bin.mptmp', 0), ('save_files guest absent GameData.bin.mpbak GameData.bin.mpbad', 0),
                           ('save_files guest absent GameData.bin.mptmp', 1), ('save_files guest present', 2), ('save_files guest maybe X', 2)):
            rc, out = judge(folder, rule)
            self.assertEqual(rc, want, rule + ': ' + out)
        d = os.path.join(folder, 'guest', 'saves/title/00010000/524d4750/data')
        torn, good = open(os.path.join(d, 'GameData.bin'), 'rb').read(), open(os.path.join(d, 'GameData.bin.mptmp'), 'rb').read()
        self.assertEqual(len(torn), len(good))
        self.assertNotEqual(torn, good)
        self.assertEqual(hashlib.sha256(good).hexdigest(), before)          # the .mptmp is the good file; the test save itself was never written
        self.assertEqual(hashlib.sha256(open(src, 'rb').read()).hexdigest(), before)
        self.assertTrue(os.path.exists(os.path.join(folder, 'host', 'save-setup.txt')))
        # a symlink in the save folder is refused (exit 2, message), nothing is written through it
        import shutil, subprocess as sp
        t = tempfile.mkdtemp(prefix='setup_link_')
        dd = os.path.join(t, 'title/00010000/524d4750/data'); os.makedirs(dd)
        victim = os.path.join(t, 'victim'); open(victim, 'wb').write(b'x' * 64)
        os.symlink(victim, os.path.join(dd, 'GameData.bin'))
        r = sp.run([sys.executable, os.path.join(H, 'save_setup.py'), t, 'tear'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn('symlink', r.stderr)
        self.assertEqual(open(victim, 'rb').read(), b'x' * 64)
        shutil.rmtree(t)
        # a bad op stops the run before the game starts
        rc, folder = pair('savesetupbad', ['FAKE_SCRIPT=' + HOST, 'HARNESS_SAVE_SETUP=explode'], ['FAKE_SCRIPT=' + GUEST])
        self.assertNotEqual(rc, 0)
        self.assertFalse(os.path.exists(os.path.join(folder, 'host', 'result.json')))

    def test_failures_and_soft_rules(self):
        rc, out = judge(self.good, 'absent host /flow move/')
        self.assertEqual(rc, 1, out)
        rc, out = judge(self.good, 'count guest /flow move/ >= 3')
        self.assertEqual(rc, 1, out)
        rc, out = judge(self.good, 'expect host /never printed/ within 1s')
        self.assertEqual(rc, 1, out)
        rc, out = judge(self.good, '?expect host /never printed/')
        self.assertEqual(rc, 0, out)
        self.assertIn('WARN', out)
        rc, out = judge(self.good, 'expect nobody /x/')
        self.assertEqual(rc, 2, out)
        rc, out = judge(self.good, 'frobnicate host /x/')
        self.assertEqual(rc, 2, out)

    def test_scenario_skip_when_feature_missing(self):
        sc = os.path.join(OUT, 'skip.yaml')
        with open(sc, 'w') as f:
            f.write('name: skipme\nrequires: {strings: [SMG_MP_NOT_IN_THIS_BINARY]}\nprocesses: [{name: host}]\nrules: []\n')
        p = subprocess.run([sys.executable, os.path.join(H, 'run_scenario.py'), sc, '-o', OUT], env=ENV, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('SKIP', p.stdout)


# ---- harness-ticks (13 Nov): rules in GAME time (gs, f), frame stamps in game.log, "slow but valid"
T0 = 1_700_000_000_000


def mkrun(root, name, speed, events, secs=40, stamps=True, frame_fn=None):
    """A synthetic run folder: a game that ran at `speed` x real time for `secs` wall seconds. events: (wall seconds, text). With stamps every
    line is "HH:MM:SS.mmm @f<frame> text" (PETARI_LOG_FRAMES=1), without them plain (and no frame points at all)."""
    import datetime
    d = os.path.join(root, name)
    os.makedirs(d, exist_ok=True)
    json.dump({'name': name, 'start_ms': T0, 'end_ms': T0 + secs * 1000, 'wall_s': secs, 'exit_code': 0, 'timed_out': False}, open(os.path.join(d, 'result.json'), 'w'))
    lines = sorted(set(list(events) + [(k, f'tick {k}') for k in range(1, secs, 3)]))
    with open(os.path.join(d, 'game.log'), 'w') as f:
        for t, text in lines:
            ts = datetime.datetime.fromtimestamp(T0 / 1000 + t).strftime('%H:%M:%S.%f')[:-3]
            f.write(f'{ts} ' + (f'@f{int((frame_fn(t) if frame_fn else t * 59.94 * speed))} ' if stamps else '') + text + '\n')
    open(os.path.join(d, 'console.log'), 'w').close()
    return d


class GameTime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix='gametime_')
        # host: "room open" at wall 2 s; guest answers 4 wall seconds later. At 0.4 x real time that is 1.6 game seconds.
        cls.slow = os.path.join(cls.root, 'slow'); os.makedirs(cls.slow)
        mkrun(cls.slow, 'host', 0.4, [(2, 'room open'), (30, 'late thing')])
        mkrun(cls.slow, 'guest', 0.4, [(6, 'welcome'), (7, 'ack')])
        cls.fast = os.path.join(cls.root, 'fast'); os.makedirs(cls.fast)
        mkrun(cls.fast, 'host', 1.0, [(2, 'room open'), (30, 'late thing')])
        mkrun(cls.fast, 'guest', 1.0, [(6, 'welcome'), (7, 'ack')])
        cls.plain = os.path.join(cls.root, 'plain'); os.makedirs(cls.plain)
        mkrun(cls.plain, 'host', 1.0, [(2, 'room open')], stamps=False)
        mkrun(cls.plain, 'guest', 1.0, [(7, 'welcome')], stamps=False)

    def test_stamps_are_parsed_and_stripped(self):
        sys.path.insert(0, H)
        import hlog
        p = hlog.Proc(os.path.join(self.slow, 'host'))
        self.assertTrue(any(t == 'room open' for _, t, _ in p.events()))
        self.assertFalse(any('@f' in t for _, t, _ in p.events()))
        self.assertAlmostEqual(p.game_s(T0 + 30000), 12.0, delta=0.05)        # 30 wall s at 0.4 x
        self.assertAlmostEqual(p.wall_ms(12.0), T0 + 30000, delta=60)         # and back

    def test_game_window_is_load_independent(self):
        # "late thing" is 12 game seconds in on the slow run, 30 s in on the fast one: a 13 gs window holds on the slow run only
        for d, rc in ((self.slow, 0), (self.fast, 1)):
            r, out = judge(d, 'expect host /late thing/ within 13gs')
            self.assertEqual(r, rc, out)
        r, out = judge(self.slow, 'expect host /late thing/ within 800f')     # 800 frames = 13.3 s
        self.assertEqual(r, 0, out)
        r, out = judge(self.slow, 'expect host /late thing/ within 600f')
        self.assertEqual(r, 3, out)                                            # (fails on a starved run: INVALID, as for every rule kind)
        r, out = judge(self.slow, 'expect host /late thing/ within 8ws')      # the same line in wall seconds: 30 s, fails (INVALID: a wall rule on a slow run)
        self.assertEqual(r, 3, out)

    def test_cross_process_game_window_uses_the_receivers_clock(self):
        # guest answers 4 wall s after the host's event = 1.6 game s on the slow run
        r, out = judge(self.slow, 'expect guest /welcome/ within 2gs after host /room open/')
        self.assertEqual(r, 0, out)
        r, out = judge(self.slow, 'expect guest /welcome/ within 2s after host /room open/')    # legacy `s` between consoles stays wall: 4 s (a wall rule: INVALID on a slow run)
        self.assertEqual(r, 3, out)
        r, out = judge(self.slow, 'order host /room open/ before guest /welcome/ within 2gs')
        self.assertEqual(r, 0, out)
        r, out = judge(self.slow, 'order host /room open/ before guest /welcome/ within 1gs')
        self.assertEqual(r, 3, out)   # (a failed cross-console window on a starved run is INVALID)
        r, out = judge(self.slow, 'order guest /ack/ before host /room open/ within 1gs tol 3gs')   # host's event is 2 game s earlier: inside the tolerance
        self.assertEqual(r, 0, out)
        r, out = judge(self.slow, 'order guest /ack/ near host /room open/ within 3gs')
        self.assertEqual(r, 0, out)

    def test_game_unit_without_frame_points_fails_loudly(self):
        r, out = judge(self.plain, 'expect host /room open/ within 5gs')
        self.assertEqual(r, 1, out)
        self.assertIn('no game-frame stamps', out)
        r, out = judge(self.plain, 'expect host /room open/ within 5s')   # legacy: wall fallback, as before
        self.assertEqual(r, 0, out)

    def test_agree_roster_and_stars(self):
        d = os.path.join(self.root, 'agree'); os.makedirs(d)
        ok = [(2, '[mp] slot 1 joined name=g rejoin=0 look=1 players=2'), (3, '[mp] flow star got id=7 grand=0 by=1')]
        mkrun(d, 'host', 1.0, ok + [(39.2, '[mp] slot 1 left reason=leave (bye) players=1'), (39.5, 'desktop: terminal elapsed_ms=1')])
        mkrun(d, 'guest', 1.0, [(2, '[mp] joined slot=1 session=ab host=0 epoch=1 players=2'), (3, '[mp] flow star got id=7 grand=0 by=1'), (39.5, 'desktop: terminal elapsed_ms=1')])
        for rule in ('agree host,guest roster', 'agree host,guest stars'):
            r, out = judge(d, rule)
            self.assertEqual(r, 0, out)       # (the host's BYE line is inside the shutdown window of the run's end: not counted)
        bad = os.path.join(self.root, 'agree_bad'); os.makedirs(bad)
        mkrun(bad, 'host', 1.0, ok + [(20, '[mp] slot 2 joined name=x rejoin=0 look=1 players=3')])
        mkrun(bad, 'guest', 1.0, [(2, '[mp] joined slot=1 session=ab host=0 epoch=1 players=2')])
        r, out = judge(bad, 'agree host,guest roster')
        self.assertEqual(r, 1, out)
        self.assertIn('DISAGREE', out)
        r, out = judge(bad, 'agree host,guest stars')
        self.assertEqual(r, 1, out)
        # the automatic soft pair: a disagreement warns, never fails
        r, out = judge(bad, 'expect host /slot 1 joined/')
        self.assertEqual(r, 0, out)
        self.assertIn('SOFT-FAILED', out)

    def test_vsbase_against_the_single_player_baseline(self):
        d = os.path.join(self.root, 'vb'); os.makedirs(d)
        h = mkrun(d, 'host', 1.0, [(2, 'x')])
        json.dump({'p95_ms': 40, 'fps': 20}, open(os.path.join(h, 'frametimes.json'), 'w'))
        base = os.path.join(self.root, 'base.json'); json.dump({'p95_ms': 20, 'fps': 55, 'p50_ms': 17}, open(base, 'w'))
        def run(rule, *extra):
            p = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), d, '-q', '-e', rule] + list(extra), env=ENV, capture_output=True, text=True)
            return p.returncode, p.stdout + p.stderr
        self.assertEqual(run('vsbase host p95_ms <= 1.5', '--baseline', base)[0], 1)     # 40 > 30
        self.assertEqual(run('vsbase host p95_ms <= 2.5', '--baseline', base)[0], 0)
        self.assertEqual(run('vsbase host fps >= 0.7', '--baseline', base)[0], 1)        # 20 < 38.5: a 3 fps game fails too
        rc, out = run('vsbase host p95_ms <= 1.5')                                       # no baseline: skipped, says so
        self.assertEqual(rc, 0, out)
        self.assertIn('SKIPPED', out)

    def test_asymmetric_speeds_and_receiver_hang(self):
        d = os.path.join(self.root, 'asym'); os.makedirs(d)
        mkrun(d, 'host', 0.3, [(2, 'room open')]); mkrun(d, 'guest', 1.0, [(4, 'welcome')])
        r, out = judge(d, 'expect guest /welcome/ within 3gs after host /room open/')   # 2 wall s on the (fast) receiver = 2 gs
        self.assertEqual(r, 0, out)
        r, out = judge(d, 'expect guest /welcome/ within 1gs after host /room open/')   # the slow SENDER is not charged to the receiver: fails
        self.assertEqual(r, 3, out)                                                      # (host is starved and the rule is cross-console: INVALID)
        # a receiver that hangs (no frames from wall 10 s to 32 s) answers 28 wall s after the event but only 6 game s: the wall ceiling
        # (window / 0.25 = 26 s) refuses the stretch
        h = os.path.join(self.root, 'hang'); os.makedirs(h)
        fn = lambda t: 59.94 * (t if t < 10 else 10 if t < 32 else 10 + (t - 32))
        mkrun(h, 'host', 1.0, [(5, 'ping')]); mkrun(h, 'guest', 1.0, [(33, 'pong')], frame_fn=fn)
        r, out = judge(h, 'expect guest /pong/ within 6.5gs after host /ping/')
        self.assertNotEqual(r, 0, out)
        r, out = judge(h, 'order host /ping/ before guest /pong/ within 6.5gs')
        self.assertNotEqual(r, 0, out)
        mkrun(h, 'guest', 1.0, [(5, 'ping'), (33, 'pong')], frame_fn=fn)       # the same hang inside ONE console: the ceiling holds there too
        r, out = judge(h, 'expect guest /pong/ within 6.5gs after guest /ping/')
        self.assertNotEqual(r, 0, out)
        mkrun(h, 'guest', 1.0, [(33, 'pong')], frame_fn=fn)
        p = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), h, '-q', '-e', 'expect guest /pong/ within 6.5gs after host /ping/',
                            '-e', 'order host /ping/ before guest /pong/ within 6.5gs'], env=dict(ENV, HARNESS_WALL_CEIL_SPEED='0'), capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)    # no ceiling: the old (stretching) reading

    def test_unconverted_scenario_stays_invalid_when_starved(self):
        # emu_wifi_blip has no gs / f rules: its failures (lines that never came) on a starved machine are the machine's, not the game's
        d = os.path.join(self.root, 'unconv'); os.makedirs(d)
        mkrun(d, 'host', 0.4, [(2, 'room open')]); mkrun(d, 'guest', 0.4, [(6, 'welcome')])
        p = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), d, '-q', '--scenario', os.path.join(H, 'scenarios', 'emu_wifi_blip.yaml')],
                           env=ENV, capture_output=True, text=True)
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertIn('INVALID', p.stdout)

    def test_vsbase_bad_inputs_are_skipped_not_failed(self):
        d = os.path.join(self.root, 'vb2'); os.makedirs(d)
        h = mkrun(d, 'host', 1.0, [(2, 'x')])
        json.dump({'p95_ms': 40, 'fps': 20}, open(os.path.join(h, 'frametimes.json'), 'w'))
        good = os.path.join(self.root, 'good.json'); json.dump({'p95_ms': 20, 'fps': 55}, open(good, 'w'))
        lst = os.path.join(self.root, 'list.json'); json.dump([1, 2, 3], open(lst, 'w'))
        junk = os.path.join(self.root, 'junk.json'); open(junk, 'w').write('{not json')
        text = os.path.join(self.root, 'text.json'); json.dump({'p95_ms': 'fast'}, open(text, 'w'))
        def run(rule, base):
            p = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), d, '-q', '-e', rule, '--baseline', base], env=ENV, capture_output=True, text=True)
            return p.returncode, p.stdout + p.stderr
        rc, out = run('vsbase host p95_ms <= abc', good)       # non-numeric factor
        self.assertEqual(rc, 0, out)
        self.assertIn('SKIPPED', out)
        for base in (lst, junk, text):                          # not an object / unreadable / a non-numeric value
            rc, out = run('vsbase host p95_ms <= 1.5', base)
            self.assertEqual(rc, 0, base + out)
            self.assertIn('SKIPPED', out)

    def test_verdict_policy_is_the_old_one(self):
        # INVALID = a failed rule + a console under min_speed, whatever the rule is; a PASS stays a PASS (slow is only information)
        js = os.path.join(self.root, 'v.json')
        def verdict(d, *rules):
            cmd = [sys.executable, os.path.join(H, 'assert_log.py'), d, '-q', '--json', js] + [x for r in rules for x in ('-e', r)]
            p = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
            return p.returncode, p.stdout, json.load(open(js))
        sl = os.path.join(self.root, 'v040'); os.makedirs(sl)
        mkrun(sl, 'host', 0.4, [(2, 'room open')]); mkrun(sl, 'guest', 0.4, [(6, 'welcome')])
        for rule in ('expect host /never printed/ within 5gs', 'expect host /never printed/ within 5ws', 'expect host /never printed/', 'absent host /room open/',
                     'expect guest /never printed/ within 5gs after host /room open/', 'exit * == 5'):
            rc, out, j = verdict(sl, rule)
            self.assertEqual(rc, 3, rule + out)            # starved: INVALID for every kind of rule, game-unit windows too
            self.assertEqual(j['verdict'], 'INVALID')
        # not starved: the same failures are FAILs
        rc, out, j = verdict(self.fast, 'expect host /never printed/ within 5gs')
        self.assertEqual(rc, 1, out)
        # a PASS stays a PASS, slow or very slow, and is marked slow
        for name, sp in (('v040', 0.4), ('v010', 0.1)):
            d = os.path.join(self.root, name); os.makedirs(d, exist_ok=True)
            if name != 'v040':
                mkrun(d, 'host', sp, [(2, 'room open')]); mkrun(d, 'guest', sp, [(6, 'welcome')])
            rc, out, j = verdict(d, 'expect host /room open/ within 5gs', 'absent host /nothing/')
            self.assertEqual(rc, 0, out)
            self.assertTrue(j['slow'])
            self.assertEqual(j['verdict'], 'PASS')
        rc, out, j = verdict(self.fast, 'expect host /room open/ within 5gs')
        self.assertEqual(rc, 0, out)
        self.assertFalse(j['slow'])
        self.assertEqual(j['results'][0]['timing'], 'bounded')       # (tags are information)

    def test_old_scenarios_judge_as_before_under_starvation(self):
        # a_boot_observatory (one console, `within 45s`), mp_join, mp_four (min_speed 0.15): the rules fail (no lines); starved = INVALID, a healthy
        # machine = FAIL, and mp_four at 0.2 (above its own 0.15 line) = FAIL: exactly the pre-harness-ticks verdicts
        sys.path.insert(0, H)
        import run_scenario
        for name, speed, want in (('a_boot_observatory', 0.4, 3), ('a_boot_observatory', 1.0, 1), ('mp_join', 0.4, 3), ('mp_join', 1.0, 1),
                                  ('mp_four', 0.1, 3), ('mp_four', 0.2, 1)):
            sc_path = os.path.join(H, 'scenarios', name + '.yaml')
            sc = run_scenario.load_scenario(sc_path)
            d = os.path.join(self.root, f'old_{name}_{speed}'); os.makedirs(d)
            for pr in sc['processes']:
                mkrun(d, pr['name'], speed, [(2, 'something')])
            cmd = [sys.executable, os.path.join(H, 'assert_log.py'), d, '-q', '--scenario', sc_path]
            if sc.get('min_speed') is not None:
                cmd += ['--min-speed', str(sc['min_speed'])]
            p = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
            self.assertEqual(p.returncode, want, f'{name} at {speed}: ' + p.stdout[-600:] + p.stderr[-300:])


if __name__ == '__main__':
    unittest.main(verbosity=2)

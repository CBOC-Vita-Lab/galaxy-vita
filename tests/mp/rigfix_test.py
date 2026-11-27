#!/usr/bin/env python3
"""Self-tests of the rig itself (the verifier's "the harness can PASS while the product or the rig is broken" findings), no game needed.

  python3 tests/mp/rigfix_test.py        (uses harness_fakegame.py; about 30 s)

Item 1: run_scenario.py preflight -> INVALID (never PASS). Item 2: assert_log.py vacuity guard / soft report. Item 3: path not impaired.
"""
import json, os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
FAKE = os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py')
OUT = tempfile.mkdtemp(prefix='rigfix_test_')
ENV = dict(os.environ, HARNESS_BIN=FAKE, HARNESS_OUT=OUT, HARNESS_GAMERUN='', HARNESS_MIN_LINES='0')
ENV.pop('HARNESS_LINK_PROFILE', None)
ENV.pop('HARNESS_VITA_LIMITS', None)
sys.path.insert(0, H)
import run_scenario as rs   # noqa: E402

LINES = '0.3:[mp] a;0.6:[mp] b'
BASE = """name: %(name)s
seconds: 3
%(extra)s
processes:
  - {name: host, env: {FAKE_SCRIPT: "%(s)s"%(hostenv)s}}
  - {name: guest, env: {FAKE_SCRIPT: "%(s)s"}}
"""


def scen(name, extra='', hostenv='', rules=None):
    text = BASE % dict(name=name, extra=extra, s=LINES, hostenv=hostenv)
    if rules is None:
        rules = ['expect host /mp. a/ within 3s']
    text += 'rules:\n' + ''.join(f"  - '{r}'\n" for r in rules)
    p = os.path.join(OUT, name + '.yaml')
    open(p, 'w').write(text)
    return p


def run(path, **env):
    e = dict(ENV, **env)
    p = subprocess.run([sys.executable, os.path.join(H, 'run_scenario.py'), path, '-o', OUT], env=e, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr)[-3000:]


class Preflight(unittest.TestCase):
    def test_good_run_still_passes(self):
        rc, out = run(scen('pf_good'))
        self.assertEqual(rc, 0, out)

    def test_typo_rules_key_is_invalid(self):
        p = os.path.join(OUT, 'pf_typo.yaml')
        open(p, 'w').write(BASE % dict(name='pf_typo', extra='', s=LINES, hostenv='') + 'rule:\n  - "absent * /never/"\n')
        rc, out = run(p)
        self.assertNotEqual(rc, 0, out)
        self.assertIn('INVALID', out)
        self.assertIn('no hard rules', out)

    def test_only_soft_rules_is_invalid(self):
        rc, out = run(scen('pf_soft', rules=['?expect host /never printed/']))
        self.assertIn('INVALID', out)
        self.assertNotEqual(rc, 0, out)

    def test_early_clean_exit_is_invalid(self):
        rc, out = run(scen('pf_early', hostenv=', FAKE_EXIT_AT: "0.8"'))
        self.assertIn('INVALID', out)
        self.assertIn('early clean exit', out)

    def test_profile_on_link_false_is_invalid(self):
        rc, out = run(scen('pf_nolink', extra='link: false'), HARNESS_LINK_PROFILE='bad_wifi')
        self.assertIn('INVALID', out)
        self.assertIn('link: false', out)

    def test_link_profile_with_no_traffic_is_invalid(self):
        # the fake game sends no packets: the relay carried nothing on any pair, so the "bad wifi" run proves nothing
        rc, out = run(scen('pf_zero'), HARNESS_LINK_PROFILE='bad_wifi')
        self.assertIn('INVALID', out)
        self.assertIn('relay carried no packet', out)

    def test_direct_path_under_a_profile_is_not_impaired(self):
        # a scenario that dials by address under a bad-network profile: that UDP socket bypasses the relay -> INVALID, reason named
        p = scen('pf_direct', hostenv=', SMG_MP_AUTO_CONN: direct')
        rc, out = run(p, HARNESS_LINK_PROFILE='bad_wifi')
        self.assertIn('path not impaired', out)
        rc, out = run(p)   # without a profile it is a normal run
        self.assertNotIn('path not impaired', out)

    def test_direct_port_is_leased_and_substituted(self):
        # @DIRECT_PORT@ in an env value becomes a port from the run's own lease (never a fixed number): both consoles see the same one
        p = scen('pf_dport', hostenv=', X_PORT: "@DIRECT_PORT@"')
        rc, out = run(p)
        self.assertEqual(rc, 0, out)
        d = [x for x in os.listdir(OUT) if x.startswith('pf_dport-')][0]
        port = int(open(os.path.join(OUT, d, 'direct-port.txt')).read())
        self.assertIn(f'X_PORT={port}', open(os.path.join(OUT, d, 'host.env')).read())
        base = int(open(os.path.join(OUT, d, 'link-base.txt')).read())
        self.assertGreaterEqual(port, base + 2)   # past the two console ports

    # ---- fabricated run folders: the cases a fake game cannot produce
    def folder(self, name, procs=('host', 'guest'), wall=6.0, exit_code=0, mem_line='mem: x'):
        d = os.path.join(OUT, name)
        for n in procs:
            os.makedirs(os.path.join(d, n), exist_ok=True)
            json.dump(dict(exit_code=exit_code, timed_out=False, wall_s=wall), open(os.path.join(d, n, 'result.json'), 'w'))
            json.dump(dict(mem_line=mem_line), open(os.path.join(d, n, 'summary.json'), 'w'))
        json.dump(dict(wall_s=wall), open(os.path.join(d, 'pair.json'), 'w'))
        return d

    SC = dict(seconds=6, rules=['absent * /x/'], processes=[dict(name='host'), dict(name='guest')])

    def test_missing_result_json(self):
        d = self.folder('fab_missing', procs=('host',))
        self.assertTrue([w for w in rs.preflight(self.SC, d) if 'guest has no result.json' in w])

    def test_stray_relay_dead_and_missing_direction(self):
        d = self.folder('fab_link')
        st = dict(stray=3, runtime_s=1.0, paths={'host>guest': {'out': 5}})
        json.dump(st, open(os.path.join(d, 'link-stats.json'), 'w'))
        w = ' | '.join(rs.preflight(dict(self.SC, link='bad_wifi'), d))
        self.assertIn('stray', w)
        self.assertIn('died mid-run', w)
        self.assertIn('guest>host', w)
        os.remove(os.path.join(d, 'link-stats.json'))
        self.assertIn('no link-stats.json', ' | '.join(rs.preflight(dict(self.SC, link='bad_wifi'), d)))

    def test_vita_limits_ignored(self):
        d = self.folder('fab_vl', mem_line='mem: banks not logged | peak RSS 1 MiB')
        os.environ['HARNESS_VITA_LIMITS'] = '1'
        try:
            w = rs.preflight(self.SC, d)
        finally:
            del os.environ['HARNESS_VITA_LIMITS']
        self.assertTrue([x for x in w if 'limits were not applied' in x])
        self.assertEqual(rs.preflight(self.SC, d), [])


class Judge(unittest.TestCase):
    """Item 2: assert_log.py cannot be satisfied by a console that logged nothing; soft failures and first-anchor-only order are visible."""
    @classmethod
    def setUpClass(cls):
        cls.dir = os.path.join(OUT, 'jd')
        cmd = [os.path.join(H, 'run_pair.sh'), '-n', 'jd', '-o', OUT, '-s', '3', '-P', 'host=FAKE_SCRIPT=0.3:[mp] x serial=1;0.6:[mp] x serial=2;0.9:[mp] y serial=1',
               '-P', 'guest=FAKE_SCRIPT=0.5:[mp] y serial=1']
        subprocess.call(cmd, env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def judge(self, *rules, **env):
        e = dict(ENV, **env)
        cmd = [sys.executable, os.path.join(H, 'assert_log.py'), self.dir]
        for r in rules:
            cmd += ['-e', r]
        p = subprocess.run(cmd, env=e, capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    def test_vacuous_negative_rules_fail(self):
        for rule in ('absent * /never printed/', 'count * /never printed/ == 0', 'distinct * /never (\\d)/ <= 1'):
            rc, out = self.judge(rule)
            self.assertEqual(rc, 0, out)                       # the fake tests switch the guard off
            rc, out = self.judge(rule, HARNESS_MIN_LINES='40')
            self.assertEqual(rc, 1, rule + out)                # a 5-line console proves nothing by not printing a line
            self.assertIn('vacuous', out)

    def test_soft_failure_is_in_the_summary(self):
        rc, out = self.judge('?expect host /never printed/', 'expect host /mp. x/ within 3s')
        self.assertEqual(rc, 0, out)
        self.assertIn('SOFT-FAILED', out)

    def test_order_without_each_says_it_judged_the_first_only(self):
        rule = 'order host /x serial=(\\d+)/ before host /y serial=$1/'
        rc, out = self.judge(rule)
        self.assertEqual(rc, 0, out)
        self.assertIn('only the FIRST of 2', out)
        rc, out = self.judge(rule + ' each')   # serial=2 is never followed by y serial=2
        self.assertEqual(rc, 1, out)
        rc, out = self.judge(rule + ' first')
        self.assertEqual(rc, 0, out)

OLD_REF = os.environ.get('RIGFIX_OLD_SCENARIOS', 'mp-next')   # where the weak scenarios live (git ref); the strengthened ones are the working tree's


class StrongScenarios(unittest.TestCase):
    """Item 7: scenarios that used to PASS with the guest never joined (or with an empty log) must FAIL now."""
    def planted(self, name, host, guest, host_name='host', guest_name='guest'):
        cmd = [os.path.join(H, 'run_pair.sh'), '-n', name, '-o', OUT, '-s', '3', '-P', f'{host_name}=FAKE_SCRIPT={host}']
        if guest_name:
            cmd += ['-P', f'{guest_name}=FAKE_SCRIPT={guest}']
        else:
            cmd += ['-p', host_name]   # (a single-console scenario)
        subprocess.call(cmd, env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        d = os.path.join(OUT, name)
        for proc in (host_name, guest_name):
            if not proc or not os.path.isdir(os.path.join(d, proc)):
                continue
            rj = os.path.join(d, proc, 'result.json')
            r = json.load(open(rj)); r['wall_s'] = 400.0
            json.dump(r, open(rj, 'w'))
            for k in range(100):   # the run "lasted": enough pictures
                open(os.path.join(d, proc, 'shots', f'extra_{k:03d}.png'), 'wb').write(b'\x89PNG\r\n\x1a\n')
        return d

    def judge(self, folder, scenario_text, tag):
        p = os.path.join(OUT, f'{tag}.yaml')
        open(p, 'w').write(scenario_text)
        r = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), folder, '--scenario', p, '-q'], env=ENV, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def old(self, name):
        return subprocess.run(['git', '-C', ROOT, 'show', f'{OLD_REF}:tools/mp/harness/scenarios/{name}.yaml'], capture_output=True, text=True, check=True).stdout

    def new(self, name):
        return open(os.path.join(H, 'scenarios', name + '.yaml'), encoding='utf-8').read()

    def check(self, name, host, guest, **kw):
        d = self.planted('st_' + name, host, guest, **kw)
        rc_old, out_old = self.judge(d, self.old(name), name + '_old')
        rc_new, out_new = self.judge(d, self.new(name), name + '_new')
        self.assertEqual(rc_old, 0, 'the weak scenario was supposed to pass this broken run:\n' + out_old)
        self.assertEqual(rc_new, 1, 'the strengthened scenario passed a broken run:\n' + out_new)

    HOST_ONLY = '0.3:[mp] session started (host);0.6:[mp] flow move type=0 galaxy=EggStarGalaxy scen=1 serial=1 role=host'

    def test_counter_and_link_need_the_guest(self):
        for n in ('ingame_counter1', 'ingame_counter3', 'ingame_counter5', 'ingame_link'):
            self.check(n, self.HOST_ONLY, '0.3:[mp] nothing')

    def test_spectate_needs_the_session(self):
        for n in ('mp_spectate_out', 'mp_spectate_bubble'):
            self.check(n, '0.3:[mp] nothing', '0.3:[mp] bubble on slot=1 lives=1->0 out=0;0.6:[mp] spectate: start watching slot=0 reason=1')

    def test_netsmooth_needs_the_guest(self):
        for n in ('mp_netsmooth_bad', 'mp_netsmooth_clean'):
            self.check(n, '0.3:[mp] puppet slot=1 appear model=Luigi self=0', '0.3:[mp] nothing')

    def test_menu_probe_and_shots_need_a_session(self):
        self.check('pt_probe', '0.3:[mp] nothing', '0.3:[mp] nothing')
        self.check('pt_shots', '0.3:[mp] ui: screen fileuse', '0.3:[mp] nothing')

    def test_conntest_needs_its_screen_and_report(self):
        self.check('mp_conntest', '0.3:[mp] nothing', '', host_name='game', guest_name='')


if __name__ == '__main__':
    unittest.main(verbosity=2, warnings='ignore')

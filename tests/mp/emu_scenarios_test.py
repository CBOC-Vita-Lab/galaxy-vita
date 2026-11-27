#!/usr/bin/env python3
"""Static check of the proposed emu_*.yaml scenarios (tools/mp/harness/scenarios): the YAML loads, the link: block is a valid relay config, the rules
parse (assert_log.py does not answer 2 = bad rule), the processes/events name each other consistently, and run_scenario builds a run_pair command with -l
and the marks. It does NOT run the game (none needed): whether the rules hold on a real game is open until a binary exists.

  python3 tests/mp/emu_scenarios_test.py
"""
import glob, json, os, re, subprocess, sys, tempfile, unittest, yaml

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
sys.path.insert(0, H)
import link_relay
FILES = sorted(glob.glob(os.path.join(H, 'scenarios', 'emu_*.yaml')))
OUT = tempfile.mkdtemp(prefix='emu_scen_')
ENV = dict(os.environ, HARNESS_BIN=os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py'), HARNESS_OUT=OUT, HARNESS_WARM='0',
           HARNESS_PORT_DIR=os.path.join(OUT, 'leases'), HARNESS_PORT_FIRST='44100', HARNESS_PORT_LAST='44900')


class Scenarios(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # one tiny fake pair folder with every process name used anywhere, for the rule parser
        cls.names = set()
        for f in FILES:
            for p in yaml.safe_load(open(f))['processes']:
                cls.names.add(p['name'])
        cls.folder = os.path.join(OUT, 'dummy')
        cmd = [os.path.join(H, 'run_pair.sh'), '-n', 'dummy', '-o', OUT, '-s', '2'] + [a for n in sorted(cls.names) for a in ('-p', n)]
        subprocess.run(cmd, env=ENV, capture_output=True)

    def test_there_are_scenarios(self):
        self.assertGreaterEqual(len(FILES), 6)

    def test_each(self):
        for f in FILES:
            with self.subTest(scenario=os.path.basename(f)):
                sc = yaml.safe_load(open(f, encoding='utf-8'))
                self.assertTrue(sc['name'] and sc['description'] and sc['rules'] and sc['processes'])
                self.assertEqual(sc['name'], os.path.basename(f)[:-5])
                lk = sc['link']
                cfg = link_relay.load_config(lk)
                names = [p['name'] for p in sc['processes']]
                for ev in cfg.get('events') or []:
                    if 'on' in ev:
                        self.assertIn(ev['on'].split(' ', 1)[0], names)
                        rx = ev['on'].split(' ', 1)[1]
                        self.assertTrue(rx.startswith('/') and rx.rstrip('i').endswith('/'), rx)
                        re.compile(rx[1:rx.rstrip('i').rfind('/')])
                for who in [o.get('who') for o in cfg.get('outage') or []] + [e.get('who') for e in cfg.get('events') or []]:
                    for part in (who or '*').replace('>', ' ').split():
                        self.assertIn(part, names + ['*'])
                for k in (cfg.get('directions') or {}):
                    for part in k.split('>'):
                        self.assertIn(part, names + ['*'])
                for p in sc['processes']:
                    for kv in (p.get('env') or {}).values():
                        spec = str(kv)
                        if spec.startswith('host|') or '|' in spec and p['env'].get('HARNESS_START_ON') == kv:
                            self.assertIn(spec.split('|')[0], names)
                    inp = p.get('input')
                    if inp:
                        self.assertTrue(os.path.exists(os.path.join(H, 'inputs', inp.lstrip('@').split('+')[0].split(',')[0] + '.txt')), inp)
                # the rules parse: assert_log exits 1 (rules fail on the dummy run), never 2 (bad rule)
                rules = os.path.join(OUT, name_of(f) + '.rules')
                open(rules, 'w').write('\n'.join(sc['rules']))
                r = subprocess.run([sys.executable, os.path.join(H, 'assert_log.py'), self.folder, '-q', '-r', rules], env=ENV, capture_output=True, text=True)
                self.assertNotEqual(r.returncode, 2, r.stdout + r.stderr)
                # run_scenario builds the command (the required strings are put into a dummy binary)
                binary = os.path.join(OUT, 'bin-' + name_of(f))
                open(binary, 'wb').write(b'\0'.join(s.encode() for s in (sc.get('requires') or {}).get('strings', [])) + b'\0')
                r = subprocess.run([sys.executable, os.path.join(H, 'run_scenario.py'), '--dry-run', '-o', OUT, f], env=dict(ENV, HARNESS_BIN=binary),
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn(' -l ', r.stdout)
                self.assertIn('-P ' + names[0] + '=', r.stdout)


def name_of(f):
    return os.path.basename(f)[:-5]


if __name__ == '__main__':
    unittest.main(verbosity=2)

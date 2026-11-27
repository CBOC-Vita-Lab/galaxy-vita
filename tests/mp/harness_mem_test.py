#!/usr/bin/env python3
"""Self-test of the harness's Vita-limits mode and memory line (HARNESS_VITA_LIMITS, HARNESS_AS_MIB, HARNESS_RSS_CEILING_MIB, memsummary.py)
with the fake game (tests/mp/harness_fakegame.py): no game build needed.

  python3 tests/mp/harness_mem_test.py        (about 20 s)

Proves the harness side only: the env reaches the process, the log lines are parsed into the run summary, a binary that ignores the request is
called out, the RSS ceiling kills and fails the run, the address-space limit makes an over-allocation fail inside the process. That the game's own
heaps shrink is proven by tests/mp/vita_limits_test.sh (os_mem.cpp) and, with a game binary, by the "banks" part of the mem line.
"""
import json, os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
FAKE = os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py')
OUT = tempfile.mkdtemp(prefix='harness_mem_')
BANKS_VITA = 'mem: native MEM1 0x60000000+1800000 MEM2 0x70000000+4000000 aliases (nil)/(nil), 8 regions'
BANKS_DESK = 'mem: native MEM1 0x60000000+8000000 MEM2 0x70000000+8000000 aliases (nil)/(nil), 8 regions'
SCRIPT = ('0.1:%s;0.4:host heap boot: arena 9000 KiB, in use 5000 KiB, free 4000 KiB in 3 pieces, the game\'s new holds 100 KiB;'
          '0.5:[mp] puppet heap init napa=9500000 gddr=13000000 cache=1100000 players=-1/-2/-3;'
          '0.7:[mp] puppet heap play napa=9100000 gddr=12800000 cache=900000 players=-1/-2/-3;'
          '0.9:heap: scene 2 destroyed (a b): C heap in use 61000 KiB (+10 since the last scene destroyed), of it x')


def one_env(name, extra_env, script=SCRIPT, seconds=3, **kw):
    cmd = [os.path.join(H, 'run_one.sh'), '-n', name, '-o', OUT, '-s', str(seconds), '-e', 'FAKE_SCRIPT=' + script]
    for k, v in kw.get('game_env', {}).items():
        cmd += ['-e', f'{k}={v}']
    e = dict(os.environ, HARNESS_BIN=FAKE, HARNESS_OUT=OUT, HARNESS_WARM='0', **extra_env)
    p = subprocess.run(cmd, env=e, capture_output=True, text=True)
    return p, os.path.join(OUT, name)


class Mem(unittest.TestCase):
    def test_vita_limits_env_and_summary(self):
        p, run = one_env('vita', {'HARNESS_VITA_LIMITS': '1'}, SCRIPT % BANKS_VITA)
        env = open(os.path.join(run, 'env.txt')).read()
        for k in ('PETARI_VITA_LIMITS=1', 'GXV_HEAP_BIG=1', 'GXV_COPY_DROP=1'):
            self.assertIn(k, env)
        s = json.load(open(os.path.join(run, 'summary.json')))
        self.assertTrue(s['ok'], p.stdout)
        m = s['memory']
        self.assertEqual((m['mem1_mib'], m['mem2_mib']), (24, 64))
        self.assertEqual(m['host_heap_in_use_max_kib'], 61000)
        self.assertEqual(m['free_min'], {'napa': 8886, 'gddr': 12500, 'cache': 878})
        self.assertEqual(m['heap_errors'], [])
        self.assertIn('vita limits ON', s['mem_line'])
        self.assertIn('mem: banks MEM1 24 MiB MEM2 64 MiB', p.stdout)

    def test_off_by_default(self):
        p, run = one_env('off', {}, SCRIPT % BANKS_DESK)
        self.assertNotIn('PETARI_VITA_LIMITS', open(os.path.join(run, 'env.txt')).read())
        self.assertIn('vita limits OFF', json.load(open(os.path.join(run, 'summary.json')))['mem_line'])

    def test_binary_that_ignores_the_request_is_called_out(self):
        p, run = one_env('ignored', {'HARNESS_VITA_LIMITS': '1'}, SCRIPT % BANKS_DESK)
        self.assertIn('REQUESTED BUT THE BINARY IGNORED IT', json.load(open(os.path.join(run, 'summary.json')))['mem_line'])

    def test_heap_error_lines_are_counted(self):
        p, run = one_env('err', {'HARNESS_VITA_LIMITS': '1'}, SCRIPT % BANKS_VITA + ';1.0:heap error: heap 0x1234 size 4194304 alignment 32')
        m = json.load(open(os.path.join(run, 'summary.json')))['memory']
        self.assertEqual(len(m['heap_errors']), 1)
        self.assertIn('heap error', m['heap_errors'][0])

    def test_rss_ceiling_kills_and_fails(self):
        p, run = one_env('rss', {'HARNESS_RSS_CEILING_MIB': '120'}, '9:idle', seconds=6, game_env={'FAKE_ALLOC_MB': 400})
        s = json.load(open(os.path.join(run, 'summary.json')))
        self.assertTrue(s['memory_ceiling_exceeded'])
        self.assertFalse(s['ok'])
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('RSS ceiling 120 MiB EXCEEDED', s['mem_line'])
        self.assertIn('memory ceiling exceeded', open(os.path.join(run, 'console.log')).read())

    def test_ceiling_not_hit_passes(self):
        p, run = one_env('rssok', {'HARNESS_RSS_CEILING_MIB': '2000'}, '9:idle', game_env={'FAKE_ALLOC_MB': 100})
        s = json.load(open(os.path.join(run, 'summary.json')))
        self.assertTrue(s['ok'], p.stdout)
        self.assertGreater(s['peak_vm_kb'], 100 * 1024)

    def test_address_space_limit_fails_the_allocation(self):
        p, run = one_env('as', {'HARNESS_AS_MIB': '250'}, '9:idle', game_env={'FAKE_ALLOC_MB': 400})
        s = json.load(open(os.path.join(run, 'summary.json')))
        self.assertEqual(s['exit_code'], 1)
        self.assertFalse(s['ok'])
        self.assertEqual(len(s['memory']['heap_errors']), 1)
        self.assertEqual(s['as_limit_mib'], 250)


if __name__ == '__main__':
    unittest.main(verbosity=2)

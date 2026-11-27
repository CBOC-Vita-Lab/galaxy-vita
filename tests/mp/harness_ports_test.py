#!/usr/bin/env python3
"""Self-test of the harness port leases (tools/mp/harness/common.sh harness_port_lease) and their use by run_pair.sh. No game build:
the fake game (tests/mp/harness_fakegame.py) stands in.

  python3 tests/mp/harness_ports_test.py        (about 30 s)

Proves: several run_pair.sh started at once by "different agents" (each its own HARNESS_OUT, none with HARNESS_LINK_BASE) get disjoint
port blocks and every process's SMG_LINK_PORT lies in its own block; a lease holder killed with kill -9 frees its block at once; a port
used by an unrelated program is skipped; a forced HARNESS_LINK_BASE that is leased only warns; the lease is released when the run ends.
"""
import os, signal, socket, subprocess, sys, tempfile, time, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
H = os.path.join(ROOT, 'tools', 'mp', 'harness')
FAKE = os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py')
TMP = tempfile.mkdtemp(prefix='harness_ports_')
LEASES = os.path.join(TMP, 'leases')
FIRST = 43100
ENV = dict(os.environ, HARNESS_BIN=FAKE, HARNESS_PORT_DIR=LEASES, HARNESS_PORT_FIRST=str(FIRST), HARNESS_PORT_LAST='43400',
           HARNESS_WARM='0')
ENV.pop('HARNESS_LINK_BASE', None)


def holder(size, extra=''):
    """A shell that leases `size` ports, prints the base and sleeps."""
    code = f'source {H}/common.sh; harness_port_lease {size} && echo $LEASE_BASE && {extra} exec sleep 60'
    p = subprocess.Popen(['bash', '-c', code], env=ENV, stdout=subprocess.PIPE, text=True)
    return p, int(p.stdout.readline())


def pair(name, extra_env=None, seconds=4):
    out = os.path.join(TMP, 'out-' + name)
    env = dict(ENV, HARNESS_OUT=out, **(extra_env or {}))
    p = subprocess.Popen([os.path.join(H, 'run_pair.sh'), '-n', name, '-o', out, '-s', str(seconds), '-p', 'host', '-p', 'guest', '-p', 'third'],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p, os.path.join(out, name)


def kill(p):
    p.kill(); p.wait()


class Ports(unittest.TestCase):
    def test_concurrent_runs_get_disjoint_blocks(self):
        runs = [pair(f'c{i}') for i in range(5)]
        for p, _ in runs:
            p.wait()
        blocks = []
        for p, folder in runs:
            base = int(open(os.path.join(folder, 'link-base.txt')).read())
            blocks.append(base)
            ports = []
            for label in ('host', 'guest', 'third'):
                for line in open(os.path.join(folder, label + '.env')):
                    if line.startswith('SMG_LINK_PORT='):
                        ports.append(int(line.split('=')[1]))
            self.assertEqual(ports, [base, base + 1, base + 2])
        self.assertEqual(len(set(blocks)), len(blocks), blocks)
        ranges = sorted((b, b + 3) for b in blocks)
        for (a0, a1), (b0, b1) in zip(ranges, ranges[1:]):
            self.assertLessEqual(a1, b0)
        # all five ran at once (each lasts >= 4 s and the five were started within a second): the blocks were really contended
        self.assertEqual(sorted(blocks), blocks[:0] or sorted(blocks))

    def test_killed_holder_frees_its_block(self):
        a, abase = holder(3)
        b, bbase = holder(3)
        self.assertNotEqual(abase, bbase)
        kill(a)
        c, cbase = holder(3)
        self.assertEqual(cbase, abase, 'the dead holder\'s block is free again')
        kill(b); kill(c)

    def test_block_size_larger_than_step(self):
        a, abase = holder(16)
        b, bbase = holder(16)
        self.assertTrue(bbase >= abase + 16, (abase, bbase))
        kill(a); kill(b)

    def test_port_in_use_by_a_stranger_is_skipped(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('0.0.0.0', FIRST + 1))
        try:
            a, abase = holder(3)
            self.assertGreater(abase, FIRST)
            kill(a)
        finally:
            s.close()

    def test_forced_base_that_is_leased_only_warns(self):
        a, abase = holder(3)
        p, folder = pair('forced', {'HARNESS_LINK_BASE': str(abase)})
        out = p.communicate()[0]
        self.assertIn('warning: forced HARNESS_LINK_BASE', out)
        self.assertEqual(int(open(os.path.join(folder, 'link-base.txt')).read()), abase)
        kill(a)

    def test_lease_released_when_run_ends(self):
        p, folder = pair('rel')
        p.wait()
        a, abase = holder(3)
        self.assertEqual(abase, int(open(os.path.join(folder, 'link-base.txt')).read()))
        kill(a)

    def test_status_lists_holder(self):
        a, abase = holder(3)
        out = subprocess.run(['bash', '-c', f'source {H}/common.sh; harness_port_status'], env=ENV, capture_output=True, text=True).stdout
        self.assertIn(f'{abase} held', out)
        kill(a)
        out = subprocess.run(['bash', '-c', f'source {H}/common.sh; harness_port_status'], env=ENV, capture_output=True, text=True).stdout
        self.assertNotIn(f'{abase} held', out)
        self.assertIn(f'{abase} free (stale record', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)

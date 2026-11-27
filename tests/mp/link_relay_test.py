#!/usr/bin/env python3
"""Self-test of the bad-network relay (tools/mp/harness/link_relay.py). No game: numbered datagrams go through the relay between plain UDP
sockets that play the consoles, and what arrives is measured against the profile.

  python3 tests/mp/link_relay_test.py        (about 60 s; run it at nice 10)

Checks: clean relay (all, in order); latency and jitter bounds; random loss rate; burst loss run lengths; reordering; duplication;
bandwidth cap (goodput, queue bound); outage window (nothing delivered inside, everything outside); per-direction settings; stray
senders dropped; NAT behaviours (cone: one source address per console for every peer; symmetric: a different one per peer; replies
return to the sender; address-restricted filter); mark-driven events (a log line starts an outage); same seed = same decisions;
profiles all load; --layout / --ports-needed agree with what run_pair.sh allocates.
"""
import json, os, socket, struct, subprocess, sys, tempfile, threading, time, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELAY = os.path.join(ROOT, 'tools', 'mp', 'harness', 'link_relay.py')
TMP = tempfile.mkdtemp(prefix='link_relay_test_')
_next = [43500]


class Net:
    """A relay on a fresh port block plus len(names) UDP sockets standing for the consoles."""
    def __init__(self, cfg, names=('a', 'b'), watch=False):
        self.names, self.n = list(names), len(names)
        self.base = _next[0]
        _next[0] += 40
        self.dir = tempfile.mkdtemp(dir=TMP)
        self.cfgfile = os.path.join(self.dir, 'cfg.json')
        json.dump(cfg, open(self.cfgfile, 'w'))
        out = subprocess.check_output([sys.executable, RELAY, '--layout', str(self.n), str(self.base), self.cfgfile], text=True)
        self.targets = {}
        for line in out.splitlines():
            i, lst = line.split(' ', 1)
            others = [j for j in range(self.n) if j != int(i)]
            for j, hp in zip(others, lst.split(',')):
                self.targets[(int(i), j)] = ('127.0.0.1', int(hp.split(':')[1]))
        self.socks = []
        for i in range(self.n):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(('127.0.0.1', self.base + i))
            s.settimeout(0.05)
            self.socks.append(s)
        self.got = {i: [] for i in range(self.n)}   # i -> [(recv time, src addr, payload)]
        self.stop = False
        self.threads = [threading.Thread(target=self._rx, args=(i,), daemon=True) for i in range(self.n)]
        ready = os.path.join(self.dir, 'ready')
        self.proc = subprocess.Popen([sys.executable, RELAY, '--base', str(self.base), '--names', ','.join(names), '--config', self.cfgfile,
                                      '--out', self.dir, '--watch-dir', self.dir, '--ready-file', ready], stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(ready):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('relay did not start: ' + self.proc.stderr.read().decode())
        for t in self.threads:
            t.start()
        self.t0 = time.monotonic()

    def _rx(self, i):
        while not self.stop:
            try:
                d, src = self.socks[i].recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            self.got[i].append((time.monotonic(), src, d))

    def send(self, i, j, seq, size=24):
        self.socks[i].sendto(struct.pack('!Id', seq, time.monotonic()) + b'x' * (size - 12), self.targets[(i, j)])

    def stream(self, i, j, count, gap, size=24):
        """count datagrams every `gap` seconds (absolute schedule, so a slow sender does not stretch it)."""
        t = time.monotonic()
        for k in range(count):
            self.send(i, j, k, size)
            t += gap
            d = t - time.monotonic()
            if d > 0:
                time.sleep(d)

    def finish(self, wait=0.6):
        time.sleep(wait)
        self.proc.terminate()
        self.proc.wait(5)
        self.stop = True
        for t in self.threads:
            t.join(1)
        for s in self.socks:
            s.close()
        try:
            self.stats = json.load(open(os.path.join(self.dir, 'link-stats.json')))
        except OSError:
            self.stats = None

    def rx(self, i):
        out = []
        for t, src, d in self.got[i]:
            if len(d) >= 12:
                seq, ts = struct.unpack('!Id', d[:12])
                out.append((seq, (t - ts) * 1000.0, ts - self.t0, src))
        return out


def run_stream(cfg, count, gap, size=24, wait=0.6):
    net = Net(cfg)
    net.stream(0, 1, count, gap, size)
    net.finish(wait)
    return net, net.rx(1)


class Relay(unittest.TestCase):
    def test_profiles_load(self):
        for name in ('lan', 'home_wifi', 'bad_wifi', 'hotspot', 'terrible', 'outage10', 'cone', 'symmetric'):
            n = int(subprocess.check_output([sys.executable, RELAY, '--ports-needed', '4', name], text=True))
            self.assertIn(n, (4, 12))

    def test_clean_relay_delivers_everything_in_order(self):
        net, rx = run_stream({'latency_ms': 0}, 600, 0.002)
        self.assertEqual([r[0] for r in rx], list(range(600)))
        self.assertLess(max(r[1] for r in rx), 30)

    def test_latency_and_jitter(self):
        net, rx = run_stream({'latency_ms': 40, 'jitter_ms': 10}, 400, 0.005)
        self.assertEqual(len(rx), 400)
        d = [r[1] for r in rx]
        self.assertGreater(min(d), 40 - 10 - 1)
        self.assertLess(max(d), 40 + 10 + 25)   # (+ scheduler slack of a loaded 2-core box)
        self.assertAlmostEqual(sum(d) / len(d), 40, delta=6)
        self.assertEqual([r[0] for r in rx], sorted(r[0] for r in rx), 'jitter alone must not reorder (keep_order)')

    def test_random_loss_rate(self):
        net, rx = run_stream({'loss_pct': 10, 'latency_ms': 1}, 4000, 0.0008)
        lost = 1 - len(rx) / 4000
        self.assertAlmostEqual(lost, 0.10, delta=0.025)
        self.assertEqual(net.stats['paths']['a>b']['loss'], 4000 - len(rx))

    def test_burst_loss_runs(self):
        net, rx = run_stream({'burst_enter_pct': 2, 'burst_len': 6, 'latency_ms': 1}, 5000, 0.0008)
        got = {r[0] for r in rx}
        runs, cur = [], 0
        for k in range(5000):
            if k not in got:
                cur += 1
            elif cur:
                runs.append(cur); cur = 0
        self.assertGreater(len(runs), 30)
        mean_run = sum(runs) / len(runs)
        self.assertGreater(mean_run, 4.0)   # bursts of mean 6 (adjacent bursts merge: a bit above)
        self.assertLess(mean_run, 9.5)
        lost = 1 - len(rx) / 5000   # expected enter*len/(1+enter*len) = 0.107
        self.assertAlmostEqual(lost, 0.107, delta=0.04)

    def test_reordering(self):
        net, rx = run_stream({'reorder_pct': 10, 'reorder_ms': 40, 'latency_ms': 5}, 1500, 0.004)
        self.assertEqual(len(rx), 1500)
        inv = sum(1 for a, b in zip(rx, rx[1:]) if b[0] < a[0])
        self.assertGreater(inv, 50)
        self.assertLess(inv, 250)
        self.assertAlmostEqual(net.stats['paths']['a>b']['reordered'] / 1500, 0.10, delta=0.03)

    def test_duplication(self):
        net, rx = run_stream({'dup_pct': 20, 'latency_ms': 2}, 2000, 0.001)
        self.assertAlmostEqual(len(rx) / 2000, 1.2, delta=0.05)
        self.assertEqual(len({r[0] for r in rx}), 2000)

    def test_bandwidth_cap_and_queue(self):
        # 1000-byte datagrams every 20 ms = 400 kbit/s into a 200 kbit/s pipe with a 250 ms buffer, for 4 s
        net, rx = run_stream({'bw_kbps': 200, 'queue_ms': 250, 'latency_ms': 0}, 200, 0.02, size=1000, wait=0.8)
        span = max(r[2] + r[1] / 1000.0 for r in rx) - min(r[2] for r in rx)
        goodput = len(rx) * 1000 * 8 / span / 1000.0
        self.assertAlmostEqual(goodput, 200, delta=45)
        self.assertLess(max(r[1] for r in rx), 250 + 40 + 30)   # one serialisation (40 ms) over the buffer, plus slack
        self.assertGreater(net.stats['paths']['a>b']['queue'], 40)
        self.assertEqual(net.stats['paths']['a>b']['queue'] + len(rx), 200)

    def test_outage_window(self):
        net, rx = run_stream({'latency_ms': 1, 'outage': [{'from': 1.0, 'to': 2.0}]}, 600, 0.005, wait=0.4)   # 3 s of traffic
        for seq, delay, sent, _ in rx:
            self.assertFalse(1.0 + 0.1 < sent < 2.0 - 0.1, f'delivered inside the outage: sent at {sent:.3f}')
        before = sum(1 for r in rx if r[2] < 0.95)
        after = sum(1 for r in rx if r[2] > 2.05)
        self.assertGreater(before, 150)
        self.assertGreater(after, 150)
        self.assertGreater(net.stats['paths']['a>b']['outage'], 150)

    def test_directions_differ(self):
        net = Net({'latency_ms': 1, 'directions': {'a>b': {'loss_pct': 50}}})
        t = time.monotonic()
        for k in range(1500):
            net.send(0, 1, k); net.send(1, 0, k)
            time.sleep(0.001)
        net.finish()
        ab, ba = len(net.rx(1)) / 1500, len(net.rx(0)) / 1500
        self.assertAlmostEqual(ab, 0.5, delta=0.07)
        self.assertGreater(ba, 0.97)

    def test_outage_who_isolates_one_console(self):
        net = Net({'latency_ms': 1, 'outage': [{'from': 0.5, 'to': 1.5, 'who': 'c'}]}, names=('a', 'b', 'c'))
        t_end = time.monotonic() + 2.0
        k = 0
        while time.monotonic() < t_end:
            net.send(0, 1, k); net.send(0, 2, k); net.send(2, 1, k)
            k += 1
            time.sleep(0.01)
        net.finish()
        rx_b = net.rx(1)
        c_addr = net.targets[(1, 2)][1]   # (cone NAT: b sees c as c's relay socket)
        from_c = [r for r in rx_b if r[3][1] == c_addr]
        from_a = [r for r in rx_b if r[3][1] != c_addr]
        self.assertFalse([r for r in from_c if 0.6 < r[2] < 1.4], 'c is cut off from b during its outage')
        self.assertTrue([r for r in from_c if r[2] < 0.4] and [r for r in from_c if r[2] > 1.7], 'c is heard before and after')
        self.assertTrue([r for r in from_a if 0.7 < r[2] < 1.3], 'a is not affected')
        self.assertFalse([r for r in net.rx(2) if 0.6 < r[2] < 1.4], 'nothing reaches c during its outage')

    def test_stray_sender_is_dropped(self):
        net = Net({'latency_ms': 1})
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('127.0.0.1', net.base + 30))
        for k in range(20):
            s.sendto(struct.pack('!Id', k, time.monotonic()) + b'x' * 12, net.targets[(0, 1)])
        net.stream(0, 1, 10, 0.002)
        net.finish()
        self.assertEqual(len(net.rx(1)), 10)
        self.assertEqual(net.stats['stray'], 20)
        s.close()

    def test_nat_cone_one_address_per_console(self):
        net = Net({'latency_ms': 1, 'nat': 'cone'}, names=('a', 'b', 'c'))
        for k in range(5):
            net.send(0, 1, k); net.send(0, 2, k); net.send(1, 0, k)
            time.sleep(0.01)
        net.finish()
        src_b = {r[3] for r in net.rx(1)}
        src_c = {r[3] for r in net.rx(2)}
        self.assertEqual(len(src_b), 1)
        self.assertEqual(src_b, src_c, 'a cone NAT shows every peer the same address')
        self.assertNotEqual(src_b, {r[3] for r in net.rx(0)})

    def test_nat_symmetric_address_per_peer_and_replies_return(self):
        net = Net({'latency_ms': 1, 'nat': 'symmetric'}, names=('a', 'b', 'c'))
        for k in range(5):
            net.send(0, 1, k); net.send(0, 2, k)
            time.sleep(0.01)
        time.sleep(0.2)
        src_b = {r[3] for r in net.rx(1)}
        src_c = {r[3] for r in net.rx(2)}
        self.assertEqual(len(src_b), 1)
        self.assertEqual(len(src_c), 1)
        self.assertNotEqual(src_b, src_c, 'a symmetric NAT shows every peer a different address')
        # b answers to the address it saw: the answer must come back to a, and from the address a sends to b
        net.socks[1].sendto(struct.pack('!Id', 99, time.monotonic()) + b'x' * 12, next(iter(src_b)))
        net.finish()
        back = [r for r in net.rx(0) if r[0] == 99]
        self.assertEqual(len(back), 1)
        self.assertEqual(back[0][3], net.targets[(0, 1)])

    def test_nat_restricted_filter(self):
        net = Net({'latency_ms': 1, 'nat': 'cone-restricted'})
        net.send(1, 0, 1)         # b -> a before a ever sent to b: filtered
        time.sleep(0.1)
        net.send(0, 1, 2)         # a -> b: b has not sent... b DID send to a: passes; opens a's side
        time.sleep(0.1)
        net.send(1, 0, 3)         # now a has sent to b: passes
        net.finish()
        self.assertEqual([r[0] for r in net.rx(0)], [3])
        self.assertEqual([r[0] for r in net.rx(1)], [2])

    def test_event_from_a_log_line_starts_an_outage(self):
        cfg = {'latency_ms': 1, 'events': [{'on': 'a /MARK now/', 'delay': 0.3, 'outage_s': 0.8}]}
        net = Net(cfg)
        os.makedirs(os.path.join(net.dir, 'a'))
        t_end = time.monotonic() + 2.4
        fired = None
        k = 0
        while time.monotonic() < t_end:
            if fired is None and time.monotonic() - net.t0 > 0.5:
                open(os.path.join(net.dir, 'a', 'console.log'), 'w').write('1700000000000 [mp] MARK now\n')
                fired = time.monotonic() - net.t0
            net.send(0, 1, k); k += 1
            time.sleep(0.005)
        net.finish()
        rx = net.rx(1)
        lo, hi = fired + 0.3 + 0.12, fired + 0.3 + 0.8 - 0.08   # (the log is polled every 50 ms)
        self.assertFalse([r for r in rx if lo < r[2] < hi])
        self.assertTrue([r for r in rx if r[2] < fired - 0.05])
        self.assertTrue([r for r in rx if r[2] > hi + 0.3])

    def test_same_seed_same_decisions(self):
        counts = []
        for _ in range(2):
            net, rx = run_stream({'loss_pct': 20, 'seed': 7, 'latency_ms': 0}, 800, 0.0015)
            counts.append(sorted(set(range(800)) - {r[0] for r in rx}))
        self.assertEqual(counts[0], counts[1])


class RunPair(unittest.TestCase):
    """run_pair.sh -l with a fake game and two probe bots = four consoles on one relay (no game binary)."""
    def run_pair(self, name, link, seconds=8):
        out = os.path.join(TMP, 'rp-' + name)
        env = dict(os.environ, HARNESS_BIN=os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py'), HARNESS_OUT=out, HARNESS_WARM='0',
                   HARNESS_PORT_DIR=os.path.join(TMP, 'leases'), HARNESS_PORT_FIRST='43800', HARNESS_PORT_LAST='43990')
        env.pop('HARNESS_LINK_BASE', None)
        probe = os.path.join(ROOT, 'tests', 'mp', 'link_probe.py')
        cmd = [os.path.join(ROOT, 'tools', 'mp', 'harness', 'run_pair.sh'), '-n', name, '-o', out, '-s', str(seconds), '-l', link,
               '-b', f'bot1 {sys.executable} {probe}', '-b', f'bot2 {sys.executable} {probe}']
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        return p, os.path.join(out, name)

    def test_four_consoles_through_bad_wifi(self):
        p, folder = self.run_pair('four', json.dumps({'profile': 'bad_wifi', 'nat': 'symmetric'}))
        self.assertIn('link relay pid', p.stdout, p.stdout + p.stderr)
        stats = json.load(open(os.path.join(folder, 'link-stats.json')))
        self.assertEqual(stats['nat'], 'symmetric')
        self.assertIn('bot1>bot2', stats['paths'])
        envs = open(os.path.join(folder, 'host.env')).read()
        self.assertNotIn('127.0.0.1:%s' % open(os.path.join(folder, 'link-base.txt')).read().strip(), envs.split('SMG_LINK_BCAST=')[1].split('\n')[0],
                         'a console must not be told to send straight to another console')
        probe = {b: json.load(open(os.path.join(folder, b, 'probe.json'))) for b in ('bot1', 'bot2')}
        for b, o in (('bot1', 'bot2'), ('bot2', 'bot1')):
            sent = probe[o]['sent']
            ids = [x for v in probe[b]['got'].values() for x in v]
            # sent by the other bot: they also hear host/guest? no: those are the fake game, which sends nothing
            self.assertGreater(len(ids), 0.7 * sent)
            self.assertLess(len(ids), 1.0 * sent + 20)   # bad_wifi loses ~4 to 8%, duplicates 0.2%
            self.assertGreater(stats['paths'][f'{o}>{b}']['inp'], 0.9 * sent)
        loss = 1 - len({x for v in probe['bot2']['got'].values() for x in v}) / probe['bot1']['sent']
        self.assertGreater(loss, 0.005)    # bad_wifi is lossy: 3 % random + bursts
        self.assertLess(loss, 0.25)

    def test_start_on_mark_of_another_console(self):
        out = os.path.join(TMP, 'rp-starton')
        env = dict(os.environ, HARNESS_BIN=os.path.join(ROOT, 'tests', 'mp', 'harness_fakegame.py'), HARNESS_OUT=out, HARNESS_WARM='0',
                   HARNESS_PORT_DIR=os.path.join(TMP, 'leases'), HARNESS_PORT_FIRST='43800', HARNESS_PORT_LAST='43990')
        cmd = [os.path.join(ROOT, 'tools', 'mp', 'harness', 'run_pair.sh'), '-n', 'starton', '-o', out, '-s', '4',
               '-P', 'host=FAKE_SCRIPT=2.0:[mp] the marker line', '-P', 'guest=HARNESS_START_ON=host|the marker line|0.5']
        p = subprocess.run(cmd, env=env, capture_output=True, text=True)
        folder = os.path.join(out, 'starton')
        h = json.load(open(os.path.join(folder, 'host', 'result.json')))
        g = json.load(open(os.path.join(folder, 'guest', 'result.json')))
        self.assertGreater(g['start_ms'] - h['start_ms'], 2400, p.stdout + p.stderr)
        self.assertLess(g['start_ms'] - h['start_ms'], 6000)

    def test_profile_by_name_and_env(self):
        p, folder = self.run_pair('named', 'home_wifi', seconds=5)
        self.assertTrue(os.path.exists(os.path.join(folder, 'link-stats.json')), p.stdout + p.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)

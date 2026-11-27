#!/usr/bin/env python3
"""Bad-network emulator for the multiplayer harness: a UDP impairment relay between the consoles (127.0.0.1 only).

  link_relay.py --ports-needed N CONFIG          print how many relay ports N consoles need (run_pair.sh)
  link_relay.py --layout N BASE CONFIG           print "i host:port,host:port,..." = the targets console i must broadcast to
  link_relay.py --base BASE --names a,b,c --config CONFIG [--out DIR] [--watch-dir DIR] [--stop-file F]   run the relay
  link_relay.py --list-profiles

How it sits in the path.  Console i keeps its own port BASE+i and its broadcast list (SMG_LINK_BCAST / SMG_MP_UDP_BCAST) names relay
sockets instead of the other consoles' ports.  The relay receives, applies the impairments of that direction, and sends on to the real
console from a relay socket that stands for the sender, so the receiver's replies come back through the relay too.
  nat: cone (default)     one relay socket per console (its "public address", the same for every peer)
  nat: symmetric          one relay socket per ordered pair (a different source port for every peer, as a symmetric NAT does)
  nat: cone-restricted    cone, plus an address-restricted filter: a datagram from s to o is dropped unless o sent to s in the last 30 s
Only datagrams from the console's own port are accepted (a stranger's datagram is counted as "stray" and dropped).
This models the Same Wi-Fi UDP path (VLink POSIX UDP backend, net/src/vlink_udp.cpp). It does NOT exercise the online path (STUN, hole
punching, TURN, crypto of online/): that stack opens its own sockets to the service.

CONFIG is a profile name, a path to a .json/.yaml/.yml file, or inline JSON ('{"profile": "bad_wifi", "outage": ...}').
Keys (all optional; numbers; per direction unless said):
  profile        name of a preset (see --list-profiles) used as the starting point
  nat            cone | symmetric | cone-restricted
  seed           random seed (default 1; same seed + same traffic = same decisions)
  latency_ms     one-way base delay                       jitter_ms   uniform +/- jitter around it (default: order is kept, see reorder)
  loss_pct       random loss                              burst_enter_pct / burst_len   two-state burst loss: each datagram starts a burst
                                                          with that probability; a burst lasts burst_len datagrams on average, all lost
  reorder_pct / reorder_ms   that share of datagrams is held back reorder_ms extra, so later ones overtake it (they arrive out of order)
  dup_pct        duplicated (the copy arrives up to 5 ms later)
  bw_kbps        bandwidth cap of the direction (serialisation delay + queue); queue_ms (default 250) = the buffer: more is dropped (tail drop)
  keep_order     true (default): jitter never lets a datagram overtake an earlier one; only reorder_pct does
  directions     {"host>guest": {params}, "*>host": {...}, "guest>*": {...}}   overrides, least specific first
  outage         [{"from": 40, "to": 50, "who": "*"}, ...]   everything dropped between seconds from..to (since the relay started).
                 who: "*" all paths | "host" every path to or from host (power cut / radio off) | "host>guest" one direction
  events         [{"on": "guest /RE/", "delay": 0, "outage_s": 10, "who": "host", "set": {params}, "for": 20, "each": false}, ...]
                 mark-driven: when <watch-dir>/<name>/console.log gets a line matching RE (once unless "each"), after `delay` s start an
                 outage of outage_s seconds and/or overlay `set` for `for` seconds (0/absent = until the end). {"at": 40, ...} = seconds
                 since the relay started instead of "on".
Output (--out DIR): link-relay.log (decisions summary, events), link-stats.json (per direction: in, out, drops by cause, dup, reordered,
bytes, delay percentiles). SIGTERM (or the stop file appearing) ends it after writing the stats.
"""
import argparse, heapq, json, os, random, re, select, signal, socket, sys, time

PROFILES = {
    # one-way figures. These are plausible shapes, not measurements of any particular network.
    'lan':        {'latency_ms': 1},
    'home_wifi':  {'latency_ms': 4, 'jitter_ms': 3, 'loss_pct': 0.3, 'dup_pct': 0.0},
    'bad_wifi':   {'latency_ms': 25, 'jitter_ms': 20, 'loss_pct': 3, 'burst_enter_pct': 0.5, 'burst_len': 4,
                   'reorder_pct': 1, 'reorder_ms': 30, 'dup_pct': 0.2},
    'hotspot':    {'latency_ms': 60, 'jitter_ms': 40, 'loss_pct': 2, 'burst_enter_pct': 0.3, 'burst_len': 8,
                   'reorder_pct': 0.5, 'reorder_ms': 60, 'bw_kbps': 1500, 'queue_ms': 300},
    'terrible':   {'latency_ms': 120, 'jitter_ms': 80, 'loss_pct': 10, 'burst_enter_pct': 2, 'burst_len': 10,
                   'reorder_pct': 3, 'reorder_ms': 100, 'dup_pct': 1, 'bw_kbps': 600, 'queue_ms': 400},
    'outage10':   {'latency_ms': 4, 'jitter_ms': 3, 'loss_pct': 0.3, 'outage': [{'from': 40, 'to': 50, 'who': '*'}]},
    'cone':       {'latency_ms': 4, 'nat': 'cone'},
    'symmetric':  {'latency_ms': 4, 'nat': 'symmetric'},
}
PARAMS = ('latency_ms', 'jitter_ms', 'loss_pct', 'burst_enter_pct', 'burst_len', 'reorder_pct', 'reorder_ms', 'dup_pct', 'bw_kbps',
          'queue_ms', 'keep_order')
DEFAULTS = {'latency_ms': 0.0, 'jitter_ms': 0.0, 'loss_pct': 0.0, 'burst_enter_pct': 0.0, 'burst_len': 4.0, 'reorder_pct': 0.0,
            'reorder_ms': 30.0, 'dup_pct': 0.0, 'bw_kbps': 0.0, 'queue_ms': 250.0, 'keep_order': True}
NAT_WINDOW_S = 30.0


def load_config(spec):
    """A profile name, a file path or inline JSON -> resolved dict (profile merged under the explicit keys)."""
    if isinstance(spec, dict):
        cfg = dict(spec)
    elif spec in PROFILES:
        cfg = {'profile': spec}
    elif spec.lstrip().startswith('{'):
        cfg = json.loads(spec)
    elif os.path.isfile(spec):
        text = open(spec, encoding='utf-8').read()
        if spec.endswith(('.yaml', '.yml')):
            import yaml
            cfg = yaml.safe_load(text) or {}
        else:
            cfg = json.loads(text)
    else:
        raise SystemExit(f'link_relay: unknown profile or file {spec!r} (profiles: {", ".join(PROFILES)})')
    base = {}
    if cfg.get('profile'):
        if cfg['profile'] not in PROFILES:
            raise SystemExit(f'link_relay: unknown profile {cfg["profile"]!r} (profiles: {", ".join(PROFILES)})')
        base = json.loads(json.dumps(PROFILES[cfg['profile']]))
    base.update({k: v for k, v in cfg.items() if k != 'profile'})
    base['nat'] = base.get('nat', 'cone')
    if base['nat'] not in ('cone', 'symmetric', 'cone-restricted'):
        raise SystemExit(f'link_relay: bad nat {base["nat"]!r}')
    for k in list(base):
        if k not in PARAMS and k not in ('nat', 'seed', 'directions', 'outage', 'events', 'profile'):
            raise SystemExit(f'link_relay: unknown key {k!r}')
    return base


def ports_needed(n, cfg):
    return n * (n - 1) if cfg.get('nat') == 'symmetric' else n


def layout(n, base, cfg):
    """sockets[(a, b)] = relay port that console a sends to when it talks to console b; fwd[(a, b)] = port console b sees a as."""
    sym = cfg.get('nat') == 'symmetric'
    target, fwd, own = {}, {}, {}
    if sym:
        k = base + n
        for a in range(n):
            for b in range(n):
                if a != b:
                    own[(a, b)] = k   # socket T_ab: receives what a sends towards b
                    k += 1
        for a in range(n):
            for b in range(n):
                if a != b:
                    target[(a, b)] = own[(a, b)]
                    fwd[(a, b)] = own[(b, a)]   # b sees a as T_ba (the socket b itself targets for a)
    else:
        for a in range(n):
            for b in range(n):
                if a != b:
                    target[(a, b)] = base + n + b
                    fwd[(a, b)] = base + n + a
    return target, fwd


def match_who(who, a, b, names):
    if who in (None, '*', '*>*'):
        return True
    if '>' in who:
        x, y = who.split('>', 1)
        return x in ('*', names[a]) and y in ('*', names[b])
    return who in (names[a], names[b])


class Path:
    def __init__(self, a, b, seed):
        self.a, self.b = a, b
        self.rng = random.Random(seed * 1000003 + a * 131 + b)
        self.bad_left = 0.0
        self.last_due = 0.0
        self.tx_free = 0.0
        self.stats = dict(inp=0, out=0, loss=0, burst=0, outage=0, queue=0, nat=0, dup=0, reordered=0, bytes_in=0, bytes_out=0)
        self.delays = []


class Relay:
    def __init__(self, base, names, cfg, out_dir=None, watch_dir=None, now=time.monotonic):
        self.base, self.names, self.cfg, self.n = base, names, cfg, len(names)
        self.now = now
        self.t0 = now()
        self.out_dir, self.watch_dir = out_dir, watch_dir
        self.target, self.fwd = layout(self.n, base, cfg)
        self.nat = cfg['nat']
        self.seed = int(cfg.get('seed', 1))
        self.paths = {(a, b): Path(a, b, self.seed) for a in range(self.n) for b in range(self.n) if a != b}
        self.socks = {}       # relay port -> socket
        self.by_fd = {}
        self.owner = {}       # relay port -> (a, b): "traffic a -> b arrives here" (cone: a is None = any console)
        self.src_port = {base + i: i for i in range(self.n)}
        self.heap, self.seq = [], 0
        self.overlays = []    # (until or None, who, params)
        self.outages = [(float(o['from']), float(o['to']), o.get('who', '*')) for o in cfg.get('outage') or []]
        self.timers = []      # (t, seq, kind, payload)
        self.events = [dict(e, _fired=False, _pos=0) for e in cfg.get('events') or []]
        self.last_sent = {}   # (a, b) -> time a last sent towards b (address-restricted filter)
        self.stray = 0
        self.log_f = open(os.path.join(out_dir, 'link-relay.log'), 'w') if out_dir else None
        self.dirs = {}
        for key, val in (cfg.get('directions') or {}).items():
            self.dirs[key] = val
        self._pcache = {}
        self.trace = None
        self.open_sockets()
        self.log(f'relay up: nat={self.nat} names={",".join(names)} base={base} seed={self.seed}')

    def log(self, text):
        line = f'{self.now() - self.t0:8.3f} {text}'
        if self.log_f:
            self.log_f.write(line + '\n'); self.log_f.flush()

    def open_sockets(self):
        ports = set(self.target.values()) | set(self.fwd.values())
        for p in sorted(ports):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            s.bind(('127.0.0.1', p))
            s.setblocking(False)
            self.socks[p] = s
            self.by_fd[s.fileno()] = p
        if self.nat == 'symmetric':
            for (a, b), p in self.target.items():
                self.owner[p] = (a, b)
        else:
            for b in range(self.n):
                self.owner[self.base + self.n + b] = (None, b)

    # ---- parameters of a path at a time
    def params(self, a, b, t):
        key = (a, b)
        p = self._pcache.get(key)
        if p is None:
            p = dict(DEFAULTS)
            p.update({k: self.cfg[k] for k in PARAMS if k in self.cfg})
            order = []
            for k, v in self.dirs.items():
                x, _, y = k.partition('>')
                if x in ('*', self.names[a]) and y in ('*', self.names[b]):
                    order.append((int(x != '*') + int(y != '*'), k, v))
            for _, _, v in sorted(order, key=lambda o: o[0]):
                p.update({k: v[k] for k in PARAMS if k in v})
            self._pcache[key] = p
        if self.overlays:
            q = None
            for until, who, over in self.overlays:
                if (until is None or t < until) and match_who(who, a, b, self.names):
                    q = dict(q or p); q.update(over)
            return q or p
        return p

    def in_outage(self, a, b, t):
        for lo, hi, who in self.outages:
            if lo <= t < hi and match_who(who, a, b, self.names):
                return True
        return False

    # ---- one datagram
    def handle(self, port, data, src, t):
        owner = self.owner.get(port)
        if owner is None:
            return
        a = self.src_port.get(src[1]) if src[0] == '127.0.0.1' else None
        if a is None:
            self.stray += 1
            return
        b = owner[1]
        if owner[0] is not None and owner[0] != a:
            self.stray += 1
            return
        if a == b:
            return
        self.route(a, b, data, t)

    def route(self, a, b, data, t):
        pa = self.paths[(a, b)]
        st = pa.stats
        st['inp'] += 1; st['bytes_in'] += len(data)
        self.last_sent[(a, b)] = t
        rel = t - self.t0
        if self.in_outage(a, b, rel):
            st['outage'] += 1; return
        if self.nat == 'cone-restricted':
            back = self.last_sent.get((b, a))
            if back is None or t - back > NAT_WINDOW_S:
                st['nat'] += 1; return
        p = self.params(a, b, rel)
        rng = pa.rng
        if pa.bad_left > 0:
            pa.bad_left -= 1
            st['burst'] += 1; return
        if p['burst_enter_pct'] > 0 and rng.random() * 100 < p['burst_enter_pct']:
            # geometric burst length with mean burst_len (this datagram is its first)
            L = 1
            q = 1.0 - 1.0 / max(1.0, p['burst_len'])
            while rng.random() < q and L < 10000:
                L += 1
            pa.bad_left = L - 1
            st['burst'] += 1; return
        if p['loss_pct'] > 0 and rng.random() * 100 < p['loss_pct']:
            st['loss'] += 1; return
        due = t
        if p['bw_kbps'] > 0:
            tx = len(data) * 8.0 / (p['bw_kbps'] * 1000.0)
            start = max(t, pa.tx_free)
            if start - t > p['queue_ms'] / 1000.0:
                st['queue'] += 1; return
            pa.tx_free = start + tx
            due = start + tx
        lat = p['latency_ms'] / 1000.0
        if p['jitter_ms'] > 0:
            lat += rng.uniform(-1.0, 1.0) * p['jitter_ms'] / 1000.0
        due += max(0.0, lat)
        reordered = False
        if p['reorder_pct'] > 0 and rng.random() * 100 < p['reorder_pct']:
            due += p['reorder_ms'] / 1000.0
            reordered = True
            st['reordered'] += 1
        elif p['keep_order']:
            due = max(due, pa.last_due)
        if not reordered:
            pa.last_due = max(pa.last_due, due)
        self.push(due, a, b, data, t)
        if p['dup_pct'] > 0 and rng.random() * 100 < p['dup_pct']:
            st['dup'] += 1
            self.push(due + rng.uniform(0.0, 0.005), a, b, data, t)

    def push(self, due, a, b, data, t_in):
        self.seq += 1
        heapq.heappush(self.heap, (due, self.seq, a, b, data, t_in))

    def release(self, t):
        while self.heap and self.heap[0][0] <= t:
            due, _, a, b, data, t_in = heapq.heappop(self.heap)
            s = self.socks[self.fwd[(a, b)]]
            try:
                s.sendto(data, ('127.0.0.1', self.base + b))
            except OSError:
                continue
            pa = self.paths[(a, b)]
            pa.stats['out'] += 1; pa.stats['bytes_out'] += len(data)
            if len(pa.delays) < 200000:
                pa.delays.append(t - t_in)
            if self.trace is not None:
                self.trace.append((a, b, t_in - self.t0, t - self.t0))

    # ---- timers and events
    def add_timer(self, t_rel, kind, payload):
        self.seq += 1
        heapq.heappush(self.timers, (t_rel, self.seq, kind, payload))

    def fire_action(self, ev, rel):
        who = ev.get('who', '*')
        if ev.get('outage_s'):
            self.outages.append((rel, rel + float(ev['outage_s']), who))
            self.log(f'event: outage {ev["outage_s"]} s who={who}')
        if ev.get('set'):
            until = rel + float(ev['for']) if ev.get('for') else None
            self.overlays.append((until, who, ev['set']))
            self.log(f'event: overlay {ev["set"]} for {ev.get("for", "ever")} s who={who}')

    def start_events(self):
        for ev in self.events:
            if 'at' in ev:
                self.add_timer(float(ev['at']) + float(ev.get('delay', 0)), 'action', ev)

    def poll_watch(self):
        if not self.watch_dir:
            return
        for ev in self.events:
            if 'on' not in ev or (ev['_fired'] and not ev.get('each')):
                continue
            name, _, rx = ev['on'].partition(' ')
            rx = rx.strip()
            if rx.startswith('/'):
                end = rx.rfind('/')
                flags = re.I if rx[end + 1:] == 'i' else 0
                rx = rx[1:end]
            else:
                flags = 0
            path = os.path.join(self.watch_dir, name, 'console.log')
            try:
                with open(path, 'rb') as f:
                    f.seek(ev['_pos'])
                    chunk = f.read()
            except OSError:
                continue
            nl = chunk.rfind(b'\n')
            if nl < 0:
                continue
            ev['_pos'] += nl + 1
            for line in chunk[:nl + 1].decode('utf-8', 'replace').splitlines():
                if re.search(rx, line, flags):
                    self.log(f'event: {name} printed {line.strip()[:100]!r}')
                    self.add_timer(self.now() - self.t0 + float(ev.get('delay', 0)), 'action', ev)
                    ev['_fired'] = True
                    if not ev.get('each'):
                        break

    # ---- main loop
    def step(self, max_wait=0.02):
        t = self.now()
        wait = max_wait
        if self.heap:
            wait = min(wait, max(0.0, self.heap[0][0] - t))
        if self.timers:
            wait = min(wait, max(0.0, self.timers[0][0] + self.t0 - t))
        rl, _, _ = select.select([s for s in self.socks.values()], [], [], wait)
        t = self.now()
        for s in rl:
            port = self.by_fd[s.fileno()]
            for _ in range(256):
                try:
                    data, src = s.recvfrom(65536)
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    break
                self.handle(port, data, src, t)
        t = self.now()
        while self.timers and self.timers[0][0] + self.t0 <= t:
            _, _, kind, ev = heapq.heappop(self.timers)
            self.fire_action(ev, t - self.t0)
        self.release(t)

    def run(self, stop_file=None):
        self.start_events()
        stop = []
        signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
        signal.signal(signal.SIGINT, lambda *_: stop.append(1))
        last_poll = 0.0
        while not stop:
            self.step()
            t = self.now()
            if t - last_poll >= 0.05:
                last_poll = t
                self.poll_watch()
                if stop_file and os.path.exists(stop_file):
                    break
        self.write_stats()

    def summary(self):
        out = {'nat': self.nat, 'seed': self.seed, 'stray': self.stray, 'runtime_s': round(self.now() - self.t0, 2), 'paths': {}}
        for (a, b), pa in sorted(self.paths.items()):
            if not pa.stats['inp']:
                continue
            d = sorted(pa.delays)
            pct = lambda q: round(d[min(len(d) - 1, int(q * len(d)))] * 1000, 1) if d else None
            out['paths'][f'{self.names[a]}>{self.names[b]}'] = dict(pa.stats, delay_ms_p50=pct(0.5), delay_ms_p95=pct(0.95), delay_ms_max=pct(1.0))
        return out

    def write_stats(self):
        s = self.summary()
        self.log('stats ' + json.dumps(s))
        if self.out_dir:
            json.dump(s, open(os.path.join(self.out_dir, 'link-stats.json'), 'w'), indent=1)

    def close(self):
        for s in self.socks.values():
            s.close()
        if self.log_f:
            self.log_f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ports-needed', nargs=2, metavar=('N', 'CONFIG'))
    ap.add_argument('--layout', nargs=3, metavar=('N', 'BASE', 'CONFIG'))
    ap.add_argument('--list-profiles', action='store_true')
    ap.add_argument('--base', type=int)
    ap.add_argument('--names')
    ap.add_argument('--config')
    ap.add_argument('--out')
    ap.add_argument('--watch-dir')
    ap.add_argument('--stop-file')
    ap.add_argument('--ready-file')
    a = ap.parse_args()
    if a.list_profiles:
        for k, v in PROFILES.items():
            print(f'{k:10} {json.dumps(v)}')
        return 0
    if a.ports_needed:
        n = int(a.ports_needed[0])
        print(ports_needed(n, load_config(a.ports_needed[1])))
        return 0
    if a.layout:
        n, base = int(a.layout[0]), int(a.layout[1])
        cfg = load_config(a.layout[2])
        target, _ = layout(n, base, cfg)
        for i in range(n):
            print(i, ','.join(f'127.0.0.1:{target[(i, j)]}' for j in range(n) if j != i))
        return 0
    if a.base is None or not a.names or not a.config:
        ap.error('--base, --names and --config are required to run')
    cfg = load_config(a.config)
    names = a.names.split(',')
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    r = Relay(a.base, names, cfg, a.out, a.watch_dir)
    if a.ready_file:
        open(a.ready_file, 'w').write('ready\n')
    r.run(a.stop_file)
    r.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

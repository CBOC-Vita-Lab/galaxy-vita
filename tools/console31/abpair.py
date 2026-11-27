#!/usr/bin/env python3
"""abpair.py: compare alternating console runs (on, off, on, off) on matched windows, with bootstrap confidence intervals.

  tools/console31/abpair.py --on a.log b.log --off c.log d.log [--metric frame-p95] [--bin 30] [--skip 20] [--boot 10000] [--seed 1]

Why (Strikers' note of 18 Nov, section 1.4): one console run cannot resolve 1-2 ms; heat, battery and the card's state drift between runs. Run the
arm and its control in alternation on one build (on, off, on, off), and compare the SAME stage at the SAME time into the stage, run pair by run pair.

How it reads a log (the basic log, ux0:data/smg/log.txt; lines start with HH:MM:SS.mmm):
  * `fps: 5.0 s: shown 57.5 fps; game 57.1 turns/s; N skipped; N hitches over 100 ms (longest N ms); ...` every 5 s: the samples. A frame time is
    1000 / shown fps of that 5 s window.
  * the stage is the last one named by a `flow: scene controller: ... (scene X, stage Y scenario N)` line (else by `q30lock: stage ...` / `rate: stage ...`;
    else the whole run is one stage, "all"); its Nth visit is matched with the other run's Nth visit; the window is the BIN-second slice of time since the stage's first fps sample (default 30 s); the first SKIP seconds of a stage
    are left out (loading, shader warm-up; default 20 s, as docs/CONSOLE-SESSION-3.1.md says).
  * a window needs at least 3 samples (of 6) to count.
Pairing: the 1st --on log with the 1st --off log, the 2nd with the 2nd (the run order is on, off, on, off, so neighbours in time). A window is matched when both
members of a pair have it (same stage, same bin). The unit of the bootstrap is a matched window; each gives on - off (for fps: on - off too).

Metrics (--metric):
  frame-p50 / frame-p95  ms; median / 95th percentile of the window's frame times (lower is better)
  frame-mean             ms; mean frame time of the window (lower is better)
  fps                    mean shown fps of the window (higher is better)
  hitches                hitches over 100 ms in the window (lower is better)
  pair-p95               the q30lock `game pair ... p95 X ms` value last seen in the window (lower is better; Quality 30 only)
  --regex 'text (\\d+\\.\\d+) ms'   [--lower-better|--higher-better]   any number captured from a log line (the last one in the window)
Output: matched windows, mean on, mean off, the difference with a bootstrap 95 % interval, and the verdict:
  "on better" (interval wholly on the better side), "off better", "no difference" (the interval contains 0, or fewer than --min-windows windows matched).
Exit 0 always except bad input (2). Needs only the Python 3 standard library.
"""
import argparse
import math
import random
import re
import sys

TS = re.compile(r'^(\d\d):(\d\d):(\d\d)\.(\d\d\d) ')
FPS = re.compile(r'\bfps: [\d.]+ s: shown ([\d.]+) fps;.*?(\d+) hitches over 100 ms')
FLOW = re.compile(r'\bflow: scene controller: .*\(scene (\S*), stage (\S*) scenario')
STAGE = re.compile(r'\b(?:q30lock|rate): stage ([0-9A-Za-z_:.-]+)')
PAIR = re.compile(r'game pair \(drawn \+ undrawn\) mean [\d.]+ ms = drawn [\d.]+ \+ undrawn [\d.]+, p95 ([\d.]+) ms')

METRICS = {  # name: (lower_is_better, unit)
    'frame-p50': (True, 'ms'), 'frame-p95': (True, 'ms'), 'frame-mean': (True, 'ms'),
    'fps': (False, 'fps'), 'hitches': (True, ''), 'pair-p95': (True, 'ms'),
}


def pct(vals, q):
    s = sorted(vals)
    if not s:
        return float('nan')
    k = (len(s) - 1) * q
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def read_log(path, args):
    """-> {(stage, bin): value} for the chosen metric."""
    samples = {}   # (stage, bin) -> list of (fps, hitches, extra)
    day = 0
    last_t = None
    stage = 'all'
    cur_name = None
    visits = {}
    stage_t0 = {}
    extra_regex = re.compile(args.regex) if args.regex else None
    extra_last = {}
    with open(path, 'r', errors='replace') as f:
        lines = f.read().splitlines()
    use_flow = any(FLOW.search(l) for l in lines)   # (the q30lock / rate names are only the fallback: two namings in one file would flip the stage)
    if True:
        for line in lines:
            m = TS.match(line)
            if not m:
                continue
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
            if last_t is not None and t + 43200 < last_t:   # (past midnight)
                day += 1
            last_t = t
            t += day * 86400
            fl = FLOW.search(line)
            s = None if use_flow else STAGE.search(line)
            name = None
            if fl:
                name = '%s/%s' % (fl.group(1), fl.group(2)) if fl.group(2) else fl.group(1)
            elif s:
                name = s.group(1)
            if name and name != cur_name:   # a new visit of a stage starts its own clock (the 2nd visit pairs with the other run's 2nd visit)
                cur_name = name
                visits[name] = visits.get(name, 0) + 1
                stage = '%s#%d' % (name, visits[name])
            fm = FPS.search(line)
            if fm:
                t0 = stage_t0.setdefault(stage, t)
                rel = t - t0
                if rel < args.skip:
                    continue
                key = (stage, int((rel - args.skip) // args.bin))
                samples.setdefault(key, []).append((float(fm.group(1)), int(fm.group(2))))
                continue
            val = None
            if args.metric == 'pair-p95':
                pm = PAIR.search(line)
                if pm:
                    val = float(pm.group(1))
            elif extra_regex:
                em = extra_regex.search(line)
                if em:
                    val = float(em.group(1))
            if val is not None and stage in stage_t0:
                rel = t - stage_t0[stage]
                if rel >= args.skip:
                    extra_last[(stage, int((rel - args.skip) // args.bin))] = val
    out = {}
    if args.metric == 'pair-p95' or args.regex:
        for k, v in extra_last.items():
            if k in samples:   # (windows with fps samples only: the same window rule for every metric)
                out[k] = v
        return out
    for k, v in samples.items():
        if len(v) < 3:
            continue
        fps = [a for a, _ in v if a > 0]
        ft = [1000.0 / a for a in fps]
        if not ft:
            continue
        if args.metric == 'frame-p50': out[k] = pct(ft, 0.5)
        elif args.metric == 'frame-p95': out[k] = pct(ft, 0.95)
        elif args.metric == 'frame-mean': out[k] = sum(ft) / len(ft)
        elif args.metric == 'fps': out[k] = sum(fps) / len(fps)
        elif args.metric == 'hitches': out[k] = float(sum(h for _, h in v))
    return out


def bootstrap(diffs, n, seed):
    rng = random.Random(seed)
    k = len(diffs)
    means = []
    for _ in range(n):
        means.append(sum(diffs[rng.randrange(k)] for _ in range(k)) / k)
    means.sort()
    return means[int(0.025 * (n - 1))], means[int(math.ceil(0.975 * (n - 1)))]


def compare(on_logs, off_logs, args):
    """-> dict with the result (also used by the tests)."""
    lower = not args.higher_better if (args.regex or args.metric not in METRICS) else METRICS[args.metric][0]
    if args.lower_better:
        lower = True
    diffs, ons, offs, per_pair = [], [], [], []
    for a, b in zip(on_logs, off_logs):
        ra, rb = read_log(a, args), read_log(b, args)
        common = sorted(set(ra) & set(rb))
        per_pair.append((a, b, len(common)))
        for k in common:
            ons.append(ra[k]); offs.append(rb[k]); diffs.append(ra[k] - rb[k])
    res = {'windows': len(diffs), 'per_pair': per_pair, 'lower_better': lower}
    if not diffs:
        res.update(verdict='no difference', note='no matched windows', mean_on=float('nan'), mean_off=float('nan'), diff=float('nan'), lo=float('nan'), hi=float('nan'))
        return res
    d = sum(diffs) / len(diffs)
    lo, hi = bootstrap(diffs, args.boot, args.seed)
    res.update(mean_on=sum(ons) / len(ons), mean_off=sum(offs) / len(offs), diff=d, lo=lo, hi=hi)
    if len(diffs) < args.min_windows:
        res.update(verdict='no difference', note='fewer than %d matched windows' % args.min_windows)
    elif lo > 0 or hi < 0:
        better_on = (hi < 0) if lower else (lo > 0)
        res.update(verdict='on better' if better_on else 'off better', note='')
    else:
        res.update(verdict='no difference', note='')
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--on', nargs='+', required=True, metavar='LOG')
    ap.add_argument('--off', nargs='+', required=True, metavar='LOG')
    ap.add_argument('--metric', default='frame-p95', choices=sorted(METRICS) + ['custom'])
    ap.add_argument('--regex', help='custom metric: a regex with one number group (use with --metric custom)')
    ap.add_argument('--lower-better', action='store_true')
    ap.add_argument('--higher-better', action='store_true')
    ap.add_argument('--bin', type=float, default=30.0, help='window length in s (default 30)')
    ap.add_argument('--skip', type=float, default=20.0, help='seconds of each stage left out at its start (default 20)')
    ap.add_argument('--boot', type=int, default=10000, help='bootstrap resamples (default 10000)')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--min-windows', type=int, default=4)
    args = ap.parse_args(argv)
    if len(args.on) != len(args.off):
        print('abpair: --on and --off need the same number of logs (alternating pairs)', file=sys.stderr)
        return 2
    if args.metric == 'custom' and not args.regex:
        print('abpair: --metric custom needs --regex', file=sys.stderr)
        return 2
    res = compare(args.on, args.off, args)
    unit = METRICS.get(args.metric, (True, ''))[1]
    print('metric %s (%s is better), windows of %g s after %g s of each stage' % (args.metric, 'lower' if res['lower_better'] else 'higher', args.bin, args.skip))
    for a, b, n in res['per_pair']:
        print('  pair on=%s off=%s: %d matched windows' % (a, b, n))
    if res['windows']:
        print('  %d matched windows: on %.2f %s, off %.2f %s' % (res['windows'], res['mean_on'], unit, res['mean_off'], unit))
        print('  on - off = %+.2f %s, bootstrap 95%% interval [%+.2f, %+.2f] (%d resamples)' % (res['diff'], unit, res['lo'], res['hi'], args.boot))
    print('verdict: %s%s' % (res['verdict'], (' (%s)' % res['note']) if res.get('note') else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())

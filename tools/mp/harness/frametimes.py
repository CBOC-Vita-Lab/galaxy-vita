#!/usr/bin/env python3
"""Frame pacing of a run from PETARI_FRAMETIMES (platform/desktop/main.cpp: "<frame> <ms since start> <delta ms>" lines).

  frametimes.py <run-dir | frametimes.txt> [--from S] [--to S] [--json out.json]

Prints frames, p50/p95/p99/max frame time and the number of hitches (> 50 ms) in the window [from, to] seconds since the
start (default: everything after the first 5 s). run_one.sh writes <run>/frametimes.json when the file exists.
Headless llvmpipe: the game's logic turns are paced by its own 60 Hz clock, so the numbers show game-thread stalls (loads,
network waits, locks); they are not Vita frame rates.
"""
import argparse, json, os, sys


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    k = min(len(sorted_vals) - 1, max(0, int(round(p / 100.0 * (len(sorted_vals) - 1)))))
    return sorted_vals[k]


def analyse(path, t_from=5.0, t_to=None, hitch_ms=50):
    rows = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 3:
            rows.append((int(parts[0]), int(parts[1]), int(parts[2])))
    win = [r for r in rows[1:] if r[1] >= t_from * 1000 and (t_to is None or r[1] <= t_to * 1000)]
    d = sorted(r[2] for r in win)
    span = (win[-1][1] - win[0][1]) / 1000 if len(win) > 1 else 0
    return {
        'frames': len(win), 'window_s': [t_from, t_to], 'fps': round(len(win) / span, 2) if span else None,
        'p50_ms': pct(d, 50), 'p95_ms': pct(d, 95), 'p99_ms': pct(d, 99), 'max_ms': d[-1] if d else None,
        'hitches_over_%d_ms' % hitch_ms: sum(1 for x in d if x > hitch_ms),
        'worst': sorted(((r[2], round(r[1] / 1000, 1)) for r in win), reverse=True)[:5],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--from', dest='t_from', type=float, default=5.0)
    ap.add_argument('--to', dest='t_to', type=float)
    ap.add_argument('--json')
    a = ap.parse_args()
    path = os.path.join(a.path, 'frametimes.txt') if os.path.isdir(a.path) else a.path
    if not os.path.exists(path):
        print(f'frametimes: no {path} (run with PETARI_FRAMETIMES)', file=sys.stderr)
        return 1
    r = analyse(path, a.t_from, a.t_to)
    if a.json:
        json.dump(r, open(a.json, 'w'), indent=1)
    worst = ', '.join(f'{ms} ms at {t}s' for ms, t in r['worst'])
    print(f"frametimes: {r['frames']} frames {r['fps']} fps  p50 {r['p50_ms']} ms  p95 {r['p95_ms']} ms  p99 {r['p99_ms']} ms  "
          f"max {r['max_ms']} ms  hitches>50ms {r['hitches_over_50_ms']}  (worst: {worst})")
    return 0


if __name__ == '__main__':
    sys.exit(main())

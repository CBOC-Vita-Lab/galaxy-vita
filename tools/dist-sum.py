#!/usr/bin/env python3
"""far30: turn a log with GXV_DISTCENSUS lines into a table with cumulative shares.

  python3 tools/dist-sum.py LOG [LOG ...] [--stage NAME] [--final] [--by-class] [--by-size]

Reads the lines the game writes (platform/include/port/distcensus.h):
  dist-win: <stage> turns T pictures P secs S
  dist: <stage> band <lo>-<hi> m: logic N, posed N, draws N, indices N, particles N | by class map L/P/D/I/Pt chars ... | by screen size <8px D/I ...
logic, posed and particles are means per game turn; draws and indices means per drawn picture. Several 5 s windows of a stage are averaged (logic / posed /
particles weighted by the window's turns, draws / indices by its pictures). --final uses the whole-stage lines (stage name "X(stage)") instead of the 5 s windows.
Distances are to the camera. Output: per stage a table by band and "beyond 25 m: ..." shares (what a far cut could reach at most)."""
import re, sys, argparse, collections

WIN = re.compile(r'dist-win: (\S+) turns (\d+) pictures (\d+) secs ([\d.]+)')
LINE = re.compile(r'dist: (\S+) band (\d+)-(\d+|inf) m: logic ([\d.]+), posed ([\d.]+), draws ([\d.]+), indices ([\d.]+), particles ([\d.]+)(.*)$')
CLS = re.compile(r'(map|chars|plants|items|particles|shadows|other) ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
SIZ = re.compile(r'(<8px|8-32px|32-128px|>128px) ([\d.]+)/([\d.]+)')
FIELDS = ['logic', 'posed', 'draws', 'indices', 'particles']
TURNW = {'logic', 'posed', 'particles'}
BANDLO = [0, 10, 25, 50, 100, 200]

def parse(paths):
    data = collections.OrderedDict()   # stage -> {'w': [turns, pics], 'band': {b: {field: [sum(v*w), sum(w)]}}, 'cls': {(b,c): {...}}, 'siz': {(b,s): [d,i]}}
    for p in paths:
        win = None
        for line in open(p, errors='replace'):
            m = WIN.search(line)
            if m:
                win = (m.group(1), int(m.group(2)), int(m.group(3)))
                continue
            m = LINE.search(line)
            if not m or win is None or m.group(1) != win[0]:
                continue
            st = m.group(1)
            d = data.setdefault(st, {'band': collections.defaultdict(lambda: {f: [0.0, 0.0] for f in FIELDS}), 'cls': collections.defaultdict(lambda: {f: [0.0, 0.0] for f in FIELDS}),
                                      'siz': collections.defaultdict(lambda: [[0.0, 0.0], [0.0, 0.0]]), 'wins': 0, 'turns': 0, 'pics': 0, 'lo': {}})
            if int(m.group(2)) not in BANDLO:
                continue
            b = BANDLO.index(int(m.group(2)))
            d['lo'][b] = (m.group(2), m.group(3))
            if b == 0:
                d['wins'] += 1; d['turns'] += win[1]; d['pics'] += win[2]
            vals = dict(zip(FIELDS, [float(m.group(i)) for i in range(4, 9)]))
            for f in FIELDS:
                w = win[1] if f in TURNW else win[2]
                d['band'][b][f][0] += vals[f] * w; d['band'][b][f][1] += w
            rest = m.group(9)
            cpart, _, spart = rest.partition('| by screen size')
            for c in CLS.finditer(cpart):
                for f, v in zip(FIELDS, [float(c.group(i)) for i in range(2, 7)]):
                    w = win[1] if f in TURNW else win[2]
                    d['cls'][(b, c.group(1))][f][0] += v * w; d['cls'][(b, c.group(1))][f][1] += w
            for s in SIZ.finditer(spart):
                for k in (0, 1):
                    w = win[2]
                    d['siz'][(b, s.group(1))][k][0] += float(s.group(2 + k)) * w; d['siz'][(b, s.group(1))][k][1] += w
    return data

def mean(x):
    return x[0] / x[1] if x[1] else 0.0

def report(stage, d, by_class, by_size):
    nb = max(d['band'].keys()) + 1
    rows = [{f: mean(d['band'][b][f]) for f in FIELDS} for b in range(nb)]
    tot = {f: sum(r[f] for r in rows) for f in FIELDS}
    print('== %s: %d windows, %d turns, %d drawn pictures (logic / posed / particles per turn; draws / indices per drawn picture)' % (stage, d['wins'], d['turns'], d['pics']))
    print('%-12s %9s %9s %9s %11s %10s   %s' % ('band (m)', 'logic', 'posed', 'draws', 'indices', 'particles', 'cumulative share from this band outward (logic/posed/draws/indices/particles %)'))
    for b in range(nb):
        lo, hi = d['lo'].get(b, ('?', '?'))
        out = {f: sum(rows[k][f] for k in range(b, nb)) for f in FIELDS}
        sh = ' '.join('%3.0f' % (100.0 * out[f] / tot[f] if tot[f] else 0.0) for f in FIELDS)
        print('%-12s %9.1f %9.1f %9.1f %11.0f %10.1f   from %s m: %s' % ('%s-%s' % (lo, hi), rows[b]['logic'], rows[b]['posed'], rows[b]['draws'], rows[b]['indices'], rows[b]['particles'], lo, sh))
    print('%-12s %9.1f %9.1f %9.1f %11.0f %10.1f' % ('all', tot['logic'], tot['posed'], tot['draws'], tot['indices'], tot['particles']))
    # the headline: beyond 25 m = bands from index 2
    if nb > 2:
        far = {f: sum(rows[k][f] for k in range(2, nb)) for f in FIELDS}
        far100 = {f: sum(rows[k][f] for k in range(4, nb)) for f in FIELDS} if nb > 4 else None
        pc = lambda f, x: 100.0 * x[f] / tot[f] if tot[f] else 0.0
        print('beyond 25 m: %.0f %% of draws, %.0f %% of indices, %.0f %% of posed actors, %.0f %% of logic, %.0f %% of particles' % (pc('draws', far), pc('indices', far), pc('posed', far), pc('logic', far), pc('particles', far)))
        if far100:
            print('beyond 100 m: %.0f %% of draws, %.0f %% of indices, %.0f %% of posed actors, %.0f %% of logic, %.0f %% of particles' % (pc('draws', far100), pc('indices', far100), pc('posed', far100), pc('logic', far100), pc('particles', far100)))
    if by_class:
        print('by class, beyond 25 m (share of that class\'s own total):')
        for c in ('map', 'chars', 'plants', 'items', 'particles', 'shadows', 'other'):
            t = {f: sum(mean(d['cls'][(b, c)][f]) for b in range(nb)) for f in FIELDS}
            fr = {f: sum(mean(d['cls'][(b, c)][f]) for b in range(2, nb)) for f in FIELDS}
            if not any(t.values()):
                continue
            print('  %-10s total logic %.1f posed %.1f draws %.1f indices %.0f ptcl %.1f | beyond 25 m: %s' % (c, t['logic'], t['posed'], t['draws'], t['indices'], t['particles'],
                  ' '.join('%s %.0f%%' % (f, 100.0 * fr[f] / t[f] if t[f] else 0.0) for f in ('logic', 'posed', 'draws', 'indices', 'particles'))))
    if by_size:
        print('by screen size (draws / indices per picture), all bands, and the part beyond 25 m:')
        for s in ('<8px', '8-32px', '32-128px', '>128px'):
            a = sum(mean(d['siz'][(b, s)][0]) for b in range(nb)); ai = sum(mean(d['siz'][(b, s)][1]) for b in range(nb))
            f = sum(mean(d['siz'][(b, s)][0]) for b in range(2, nb)); fi = sum(mean(d['siz'][(b, s)][1]) for b in range(2, nb))
            print('  %-9s %7.1f draws %9.0f indices | beyond 25 m: %7.1f draws %9.0f indices' % (s, a, ai, f, fi))
    print()

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--stage', help='only stages whose name contains this')
    ap.add_argument('--final', action='store_true', help='use the whole-stage lines (name(stage)) instead of the 5 s windows')
    ap.add_argument('--by-class', action='store_true')
    ap.add_argument('--by-size', action='store_true')
    a = ap.parse_args()
    data = parse(a.logs)
    shown = 0
    for st, d in data.items():
        if a.final != st.endswith('(stage)'):
            continue
        if a.stage and a.stage not in st:
            continue
        report(st, d, a.by_class, a.by_size)
        shown += 1
    if not shown:
        print('no dist: lines found (GXV_DISTCENSUS=1 needed; --final reads only the stage-end lines)')
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())

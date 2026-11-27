#!/usr/bin/env python3
"""ab-sum.py <log.txt> [more logs...] [--long out.tsv] [--matrix out.tsv]
Turns the "ab-data:" lines of GXV_AB_PLAN runs (platform/include/port/ab27.h) into TSV.
  long    one row a (stage, key): delta/noise of fps, gpu, game, fe, be, ta, f3d, p95, hitches, verdict, the per-window fps of both arms
  matrix  rows = keys, columns = stages; a cell is "fps +1.2(0.4) gpu -0.8 game -0.1 fe 0.0 : acted" (delta, noise in brackets for fps)
Several logs (several jobs) are merged: a later line of the same (stage, key) replaces the earlier one. Prints the matrix to stdout when no --matrix is given."""
import re, sys
args = sys.argv[1:]
outs = {}
logs = []
i = 0
while i < len(args):
    if args[i] in ('--long', '--matrix') and i + 1 < len(args):
        outs[args[i][2:]] = args[i + 1]; i += 2
    else:
        logs.append(args[i]); i += 1
if not logs:
    sys.exit(__doc__)
METRICS = ['fps', 'gpu', 'game', 'fe', 'be', 'ta', 'f3d', 'p95', 'hit']
rows = {}
order_stage, order_key = [], []
for path in logs:
    for line in open(path, errors='replace'):
        j = line.find('ab-data:')
        if j < 0:
            continue
        kv = dict(re.findall(r'(\w+)=(\S+)', line[j + 8:].replace('key=', 'key=', 1)))
        # the key text may hold spaces? it never does (entries are trimmed, '+' joins); a name with '=' is split at the first '=' of "key=GXV_X=0|1"
        m = re.search(r'key=(\S+)', line)
        if not m:
            continue
        key = m.group(1)
        stage = kv.get('stage', '?')
        d = {'stage': stage, 'key': key, 'verdict': kv.get('verdict', '?'), 'offfps': kv.get('offfps', ''), 'onfps': kv.get('onfps', '')}
        for mt in METRICS:
            v = kv.get(mt, '-')
            if v == '-' or '/' not in v:
                d[mt] = (None, None)
            else:
                a, b = v.split('/', 1)
                d[mt] = (float(a), None if b in ('?', '') else float(b))
        rows[(stage, key)] = d
        if stage not in order_stage: order_stage.append(stage)
        if key not in order_key: order_key.append(key)

def f(v, nd=2, sign=True):
    if v is None: return '-'
    return ('%+.*f' if sign else '%.*f') % (nd, v)

lines = ['stage\tkey\t' + '\t'.join('d_' + m + '\tn_' + m for m in METRICS) + '\tverdict\toff_fps\ton_fps']
for (stage, key), d in sorted(rows.items(), key=lambda kv: (order_stage.index(kv[0][0]), order_key.index(kv[0][1]))):
    cells = []
    for m in METRICS:
        a, b = d[m]
        cells += [f(a, 3), f(b, 3, False)]
    lines.append('\t'.join([stage, key] + cells + [d['verdict'], d['offfps'], d['onfps']]))
long_txt = '\n'.join(lines) + '\n'

mat = ['key\t' + '\t'.join(order_stage)]
for key in order_key:
    cells = []
    for st in order_stage:
        d = rows.get((st, key))
        if not d:
            cells.append('')
            continue
        fa, fb = d['fps']
        cells.append('fps %s(%s) gpu %s game %s fe %s : %s' % (f(fa, 1), f(fb, 1, False), f(d['gpu'][0]), f(d['game'][0]), f(d['fe'][0]), d['verdict']))
    mat.append(key + '\t' + '\t'.join(cells))
mat_txt = '\n'.join(mat) + '\n'
if 'long' in outs: open(outs['long'], 'w').write(long_txt)
if 'matrix' in outs: open(outs['matrix'], 'w').write(mat_txt)
else: sys.stdout.write(mat_txt)
sys.stderr.write('%d (stage,key) results, %d stages, %d keys\n' % (len(rows), len(order_stage), len(order_key)))

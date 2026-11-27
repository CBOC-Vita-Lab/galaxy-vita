#!/usr/bin/env python3
# cgmiss.py <callgrind.out[-NN]> [--base other.out] [--pics N] [--top N] [--lines] [--filt regex]
# Self D1 read-miss (D1mr), LL read-miss (DLmr), write misses per function and per source line (callgrind --dump-line=yes), per picture.
# --base: subtract another run (e.g. a shorter run: removes start-up; pics = difference of picture counts).
import sys, re, collections, argparse
ap = argparse.ArgumentParser(); ap.add_argument('f'); ap.add_argument('--base'); ap.add_argument('--pics', type=float, default=1)
ap.add_argument('--top', type=int, default=25); ap.add_argument('--lines', action='store_true'); ap.add_argument('--filt', default=r'platform/vgx/src')
ap.add_argument('--sort', default='DLmr'); ap.add_argument('--fn', default=''); a = ap.parse_args()
EV = ['Ir','Dr','Dw','I1mr','D1mr','D1mw','ILmr','DLmr','DLmw']
def parse(path):
    names = {}; files = {}; fn = None; fl = None; fi = None
    fnc = collections.defaultdict(lambda: [0]*9); lnc = collections.defaultdict(lambda: [0]*9)
    last = 0; pend = False; fnfile = {}
    for line in open(path):
        line = line.rstrip('\n')
        m = re.match(r'(fl|fi|fe)=\((\d+)\)(?: (.*))?', line)
        if m:
            if m.group(3): files[m.group(2)] = m.group(3)
            fi = files.get(m.group(2))
            if m.group(1) == 'fl': fl = fi
            continue
        m = re.match(r'fn=\((\d+)\)(?: (.*))?', line)
        if m:
            if m.group(2): names[m.group(1)] = m.group(2)
            fn = names[m.group(1)]; last = 0; fi = fl; fnfile.setdefault(fn, fl); continue
        m = re.match(r'cfn=\((\d+)\)(?: (.*))?', line)
        if m:
            if m.group(2): names[m.group(1)] = m.group(2)
            continue
        m = re.match(r'c(fi|fl)=\((\d+)\)(?: (.*))?', line)
        if m:
            if m.group(3): files[m.group(2)] = m.group(3)
            continue
        if line.startswith('calls='): pend = True; continue
        if line and (line[0].isdigit() or line[0] in '+-*'):
            p = line.split(); t = p[0]
            if t == '*': ln = last
            elif t[0] in '+-': ln = last + int(t)
            else: ln = int(t)
            last = ln
            if pend: pend = False; continue
            c = [int(x) for x in p[1:10]]; c += [0]*(9-len(c))
            for i in range(9): fnc[fn][i] += c[i]
            k = (fi or '?', ln) if not a.fn else (fi or '?', ln)
            if a.fn and not re.search(a.fn, fn or ''): continue
            for i in range(9): lnc[k][i] += c[i]
    return fnc, lnc, fnfile
F, L, FF = parse(a.f)
if a.base:
    F2, L2, _ = parse(a.base)
    for k, v in F2.items():
        for i in range(9): F[k][i] -= v[i]
    for k, v in L2.items():
        for i in range(9): L[k][i] -= v[i]
si = EV.index(a.sort)
filt = re.compile(a.filt)
def short(s): return s.replace('platform/vgx/src/', '')[-60:]
tot = [sum(v[i] for v in F.values()) for i in range(9)]
print("TOTAL per picture: Ir %.0f  D1mr %.0f  DLmr %.0f  D1mw %.0f DLmw %.0f" % (tot[0]/a.pics, tot[4]/a.pics, tot[7]/a.pics, tot[5]/a.pics, tot[8]/a.pics))
if not a.lines:
    rows = [(fn, v) for fn, v in F.items() if filt.search(FF.get(fn) or '')]
    rows.sort(key=lambda kv: -kv[1][si])
    print("%9s %9s %9s %9s  %s" % ('D1mr/p', 'DLmr/p', 'Ir/p', 'D1mw/p', 'function'))
    for fn, v in rows[:a.top]:
        print("%9.1f %9.1f %9.0f %9.1f  %s" % (v[4]/a.pics, v[7]/a.pics, v[0]/a.pics, v[5]/a.pics, fn[:110]))
else:
    rows = [(k, v) for k, v in L.items() if filt.search(k[0])]
    rows.sort(key=lambda kv: -kv[1][si])
    print("%9s %9s %9s  %s" % ('D1mr/p', 'DLmr/p', 'Ir/p', 'file:line'))
    for k, v in rows[:a.top]:
        print("%9.1f %9.1f %9.0f  %s:%d" % (v[4]/a.pics, v[7]/a.pics, v[0]/a.pics, short(k[0]), k[1]))

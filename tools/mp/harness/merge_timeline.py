#!/usr/bin/env python3
"""Merged timeline of several game / bot processes.

  merge_timeline.py <pair-folder | run-folder...> [-o timeline.txt] [-g REGEX] [--all]

Lines: "+SSS.mmm  name     text" (seconds since the first process started). By default only the interesting lines
(-g, default: [mp], port:, tour:, harness:, crash, Assertion, FATAL, boot:, desktop:) go to the timeline; --all keeps everything.
"""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hlog

DEFAULT = r'\[mp\]|\[OS\] port:|tour:|harness:|crash|Assert|FATAL|OSPanic|boot:|desktop:|mp_bot|\[bot\]|\[vlink\]|\[net\]'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('-g', '--grep', default=DEFAULT)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    procs = hlog.find_procs(a.paths)
    if not procs:
        sys.exit('merge_timeline: no run folders')
    t0, rows = hlog.timeline(procs)
    rx = None if a.all else re.compile(a.grep)
    width = max(len(p.name) for p in procs)
    out = open(a.out, 'w', encoding='utf-8') if a.out else sys.stdout
    n = 0
    for t, name, src, text in rows:
        if rx and not rx.search(text):
            continue
        out.write(f"{'+%.3f' % ((t - t0) / 1000):>10}  {name:<{width}}  {text}\n")
        n += 1
    if a.out:
        out.close()
        print(f'merge_timeline: {n} lines from {len(procs)} processes -> {a.out}')


if __name__ == '__main__':
    main()

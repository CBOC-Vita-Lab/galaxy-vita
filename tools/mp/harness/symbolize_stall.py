#!/usr/bin/env python3
"""Symbolizes the thread dumps the game prints on a stall (PETARI_STALL_S) or with PETARI_THREADS: "bt: <hex> <hex> ..."
lines in console.log -> one function per frame (llvm-addr2line against the run's binary snapshot). Frames outside the game
image (libc, libc++) are left out; wait-loop frames (btSignal, waitForCpu, SelectThread, OSSleepThread) are folded.

  symbolize_stall.py <run-dir> [-o stall.txt]
"""
import argparse, os, re, subprocess, sys

FOLD = ('btSignal', 'waitForCpu', 'SelectThread', 'OSSleepThread', 'hostThreadMain', 'defaultThreadMain')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    binary = open(os.path.join(a.run, 'binary.txt')).readline().strip()
    lines = [l.split(' ', 1)[1].rstrip('\n') if ' ' in l else l for l in open(os.path.join(a.run, 'console.log'), errors='replace')]
    addrs = sorted({x for l in lines if l.lstrip().startswith('bt:') for x in l.split()[1:] if re.fullmatch(r'4[89a-f][0-9a-f]{6}', x)})
    if not addrs:
        return 0
    names = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', binary] + ['0x' + x for x in addrs],
                           capture_output=True, text=True).stdout.splitlines()[0::2]
    sym = dict(zip(addrs, names))
    out = []
    for l in lines:
        if l.lstrip().startswith('bt:'):
            frames = [sym[x] for x in l.split()[1:] if x in sym]
            frames = [f for f in frames if not f.startswith(FOLD)]
            out += ['    ' + f for f in frames]
        elif l.startswith(('thread 0x', 'progress (stall)', 'vgx: published', 'threads:')):
            out.append(l)
    text = '\n'.join(out) + '\n'
    if a.out:
        open(a.out, 'w').write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())

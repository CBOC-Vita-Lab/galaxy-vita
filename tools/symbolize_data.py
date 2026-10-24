#!/usr/bin/env python3
"""Replaces libgame.so addresses (0x98000000 + offset) in a log with the
symbols that contain them (data or code), using llvm-nm output.

  symbolize_data.py <libgame.so> <nm.exe> < log > log.sym
"""
import bisect
import re
import subprocess
import sys

BASE = 0x98000000

lib, nm = sys.argv[1], sys.argv[2]
out = subprocess.run([nm, '-C', '-n', '--defined-only', lib], capture_output=True, text=True).stdout
addrs, names = [], []
for line in out.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3:
        try:
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
        except ValueError:
            pass

cache = {}


def sym(m):
    v = int(m.group(0), 16)
    if not (BASE <= v < BASE + 0x08000000):
        return m.group(0)
    if v in cache:
        return cache[v]
    off = v - BASE
    i = bisect.bisect_right(addrs, off) - 1
    s = names[i] if i >= 0 else '?'
    if i >= 0 and off != addrs[i]:
        s += '+%#x' % (off - addrs[i])
    cache[v] = s
    return s


for line in sys.stdin:
    sys.stdout.write(re.sub(r'0x9[89a-f][0-9a-f]{6}', sym, line))

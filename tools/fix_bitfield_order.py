#!/usr/bin/env python3
"""Mirrors the declaration order of 1-bit/n-bit `unsigned` bitfield runs for
little-endian builds so that whole-word access (masks, shifts, u32 overlays)
sees the same bit positions as CodeWarrior's big-endian allocation.

For each target the run of consecutive `unsigned name : N;` lines (optionally
with trailing comments) starting at the first bitfield after `anchor` is
split into 32-bit groups; each group is emitted reversed under
`#if !defined(__MWERKS__) && !defined(__BIG_ENDIAN__)`, with padding for a
partially filled group (CodeWarrior leaves the low bits unused).
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'decomp')
FIELD = re.compile(rb'^(\s*)unsigned\s+(\w+)\s*:\s*(\d+)\s*;')

TARGETS = [
    ('include/Game/Player/J3DModelX.hpp', b'struct Flags {'),
    ('include/Game/Player/Mario.hpp', b'struct MovementStates {'),
    ('include/Game/Player/Mario.hpp', b'struct DrawStates {'),
    ('include/Game/Player/MarioActor.hpp', b'/* 0xB98 */ u32 _B98;'),
]


def mirror(path, anchor):
    full = os.path.join(ROOT, path)
    lines = open(full, 'rb').read().split(b'\n')
    start = next(i for i, l in enumerate(lines) if anchor in l)
    i = start + 1
    while not FIELD.match(lines[i]):
        i += 1
    first = i
    run = []
    while i < len(lines) and (FIELD.match(lines[i]) or lines[i].strip().startswith(b'/*') and b'*/' in lines[i] and not lines[i].strip().endswith(b';')):
        run.append(lines[i])
        i += 1
    if any(b'MWERKS' in l for l in lines[max(0, first - 3):first]):
        print('already mirrored:', path, anchor)
        return
    fields = [l for l in run if FIELD.match(l)]
    indent = FIELD.match(fields[0]).group(1)
    groups, cur, bits = [], [], 0
    for l in fields:
        w = int(FIELD.match(l).group(3))
        if bits + w > 32:
            groups.append((cur, bits))
            cur, bits = [], 0
        cur.append(l)
        bits += w
    groups.append((cur, bits))
    le = []
    for g, used in groups:
        if used < 32:
            le.append(indent + b'unsigned : %d;' % (32 - used))
        le.extend(reversed(g))
    block = [b'#if defined(__MWERKS__) || defined(__BIG_ENDIAN__)'] + run + [b'#else'] + le + [b'#endif']
    lines[first:first + len(run)] = block
    open(full, 'wb').write(b'\n'.join(lines))
    print('mirrored %d fields in %d group(s): %s %s' % (len(fields), len(groups), path, anchor.decode()))


if __name__ == '__main__':
    for p, a in TARGETS:
        mirror(p, a)

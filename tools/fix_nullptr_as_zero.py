#!/usr/bin/env python3
"""The decomp's types.h used `#define nullptr 0` under CodeWarrior, so some code
uses `nullptr` as an integer/bool zero.  For every compiler error that involves
std::nullptr_t, replace `nullptr` with `0` on the reported line (0 is valid in
both pointer and integer contexts)."""
import re
import sys
import os

log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
root = sys.argv[2]
pat = re.compile(r'^(?P<file>[^\s:]+(?::[^\s:]+)?):(?P<line>\d+):\d+: error: [^\n]*nullptr_t', re.M)
targets = {}
for m in pat.finditer(log):
    f = m.group('file').replace('\\', '/')
    idx = f.find('/decomp/')
    if idx < 0:
        continue
    rel = f[idx + len('/decomp/'):]
    targets.setdefault(rel, set()).add(int(m.group('line')))

changed = 0
for rel, lines in targets.items():
    p = os.path.join(root, rel)
    with open(p, 'rb') as fh:
        text = fh.read().decode('utf-8', 'surrogateescape')
    rows = text.split('\n')
    for ln in lines:
        i = ln - 1
        if 'nullptr' in rows[i]:
            rows[i] = re.sub(r'\bnullptr\b', '0', rows[i])
            changed += 1
    with open(p, 'wb') as fh:
        fh.write('\n'.join(rows).encode('utf-8', 'surrogateescape'))
print('patched', changed, 'lines in', len(targets), 'files')

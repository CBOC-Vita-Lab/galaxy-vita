#!/usr/bin/env python3
"""Lists pointer -> 32-bit integer casts in the decomp, which truncate on the
64-bit port. The normal build hides them (-fms-extensions,
-Wno-microsoft-cast); this re-runs every compile command syntax-only with
those diagnostics enabled.

  find_ptr_casts.py [build-android/compile_commands.json] > casts.txt
"""
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

db = sys.argv[1] if len(sys.argv) > 1 else 'build-android/compile_commands.json'
entries = [e for e in json.load(open(db)) if '/decomp/' in e['file'].replace('\\', '/')]

ENABLE = ['-Wmicrosoft-cast', '-Wpointer-to-int-cast', '-Wvoid-pointer-to-int-cast',
          '-Wshorten-64-to-32', '-fno-caret-diagnostics']
PATTERN = re.compile(r'cast (?:to smaller integer type|from pointer to smaller type)|loses information|pointer-to-int')


def run(e):
    args = e['arguments'] if 'arguments' in e else shlex.split(e['command'], posix=False)
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a == '-o':
            skip = True
            continue
        if a in ('-c',) or a.startswith('-Wno-microsoft-cast') or a.startswith('-Wno-pointer-to-int-cast') \
                or a.startswith('-Wno-void-pointer-to-int-cast'):
            continue
        out.append(a.strip('"'))
    out += ['-fsyntax-only'] + ENABLE
    r = subprocess.run(out, cwd=e['directory'], capture_output=True, text=True)
    return [l for l in r.stderr.splitlines() if 'warning' in l and PATTERN.search(l)]


seen = set()
with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
    for lines in ex.map(run, entries):
        for l in lines:
            l = l.replace('\\', '/')
            l = re.sub(r'^.*?/port/', '', l)
            if l not in seen:
                seen.add(l)
                print(l, flush=True)

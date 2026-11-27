#!/usr/bin/env python3
"""merge_gate.py NAME [NAME...] | --all [--root /home/claude/g/new]
Run before merging an agent's patch. For each agent directory <root>/<NAME>/ it looks at every recheck*.out / recheck*.log /
*.summary.txt there and /tmp/recheck-<NAME>*(.summary.txt|.out), reads the LAST `FAILED n of m` line of each, and takes the
NEWEST such check. Verdict:
  OK     newest check is not older than PATCH.diff and every FAIL line is one of the two known false failures
  STALE  the check is older than PATCH.diff (the patch changed after it)
  FAIL   a FAIL line that is not a known false failure (the first is quoted)
  NOCHK  no check file / no `FAILED n of m` line (never run, still queued, or cut short)
  NOPATCH no PATCH.diff
Exit 0 only when every named agent is OK. Known false failures: Map/FileSelector.cpp, NameObj/NameObjFactory.cpp.
Also noted (not failing): NODEF-IN-BUILD lines of linkcheck (a symbol defined only in a unit the build list lacks)."""
import glob, os, re, sys
KNOWN = ("Map/FileSelector.cpp", "NameObj/NameObjFactory.cpp")
FAILED = re.compile(r'^FAILED (\d+) of (\d+)')
FAILLINE = re.compile(r'^\[\d+/\d+\] FAIL (\S+) (\S+)')

def read(p):
    try: return open(p, errors="replace").read().splitlines()
    except OSError: return []

def judge(name, root):
    d = os.path.join(root, name)
    patch = os.path.join(d, "PATCH.diff")
    files = set()
    for pat in ("recheck*.out", "recheck*.log", "*.summary.txt"):
        files.update(glob.glob(os.path.join(d, pat)))
    files.update(glob.glob("/tmp/recheck-%s*.summary.txt" % name))
    files.update(glob.glob("/tmp/recheck-%s*.out" % name))
    files = [f for f in files if os.path.isfile(f)]
    checks = []
    for f in files:
        lines = read(f)
        last = [l for l in lines if FAILED.match(l)]
        if last: checks.append((os.path.getmtime(f), f, lines, last[-1]))
    if not os.path.isfile(patch): return "NOPATCH", "no PATCH.diff in %s" % d, None
    if not checks:
        return "NOCHK", "no recheck file with a `FAILED n of m` line (%d candidate files)" % len(files), None
    mt, f, lines, last = max(checks)
    pm = os.path.getmtime(patch)
    bad = [l for l in lines if FAILLINE.match(l) and not any(k in l for k in KNOWN)]
    nodef = sum(1 for l in lines if l.startswith("NODEF-IN-BUILD"))
    note = " [%s; %s%s]" % (os.path.basename(f), last, "; %d NODEF-IN-BUILD" % nodef if nodef else "")
    if bad: return "FAIL", "not a known false failure: %s%s" % (bad[0][:110], note), f
    if mt < pm: return "STALE", "check is %d min older than PATCH.diff%s" % ((pm - mt) / 60, note), f
    return "OK", "ok%s" % note, f

def main():
    a = sys.argv[1:]
    root = "/home/claude/g/new"
    if "--root" in a:
        k = a.index("--root"); root = a[k + 1]; del a[k:k + 2]
    if "--all" in a:
        names = sorted(n for n in os.listdir(root) if os.path.isdir(os.path.join(root, n)) and os.path.isfile(os.path.join(root, n, "PATCH.diff")))
    else:
        names = [x for x in a if not x.startswith("--")]
    if not names: print(__doc__); return 2
    rc = 0
    for n in names:
        v, why, _ = judge(n, root)
        print("%-8s %-22s %s" % (v, n, why))
        if v != "OK": rc = 1
    return rc
sys.exit(main())

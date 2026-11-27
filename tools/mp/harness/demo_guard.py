#!/usr/bin/env python3
"""Hash every file under a run folder's saves/ and mp/ (and any *.cfg / *.ini beside them) and compare two snapshots.

  demo_guard.py snap RUNDIR OUT.json       write the snapshot (path -> sha256, size)
  demo_guard.py diff BEFORE.json AFTER.json  exit 0 when identical (no file made, changed or removed), 1 otherwise

The puppet demo (SMG_MP_DEMO) must not write a save, a co-op file, mp.cfg or flow.cfg or switch the save root; save_guard.py only
covers *.bin, this covers everything the game could write there.
"""
import hashlib, json, os, sys

SKIP_DIRS = {"shots", "shader-cache"}
SKIP_FILES = {"console.log", "game.log", "env.raw", "env.txt", "cmd.txt", "proc.csv", "frametimes.txt", "result.json", "summary.json",
              "save-before.json", "save-after.json", "save-guard.json", "crash.txt", "log.txt"}


def snap(run):
    out = {}
    for sub in ("saves", "mp"):   # what the game may write: the single-player save root and the multiplayer root (mp.cfg, flow.cfg, co-op files)
        for root, dirs, files in os.walk(os.path.join(run, sub)):
            for f in files:
                p = os.path.join(root, f)
                h = hashlib.sha256()
                with open(p, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                out[os.path.relpath(p, run)] = {"sha256": h.hexdigest(), "size": os.path.getsize(p)}
    for f in os.listdir(run):   # and any settings file beside them (*.cfg, *.ini, *.json written by the game itself, not the harness's own logs)
        if f.endswith((".cfg", ".ini")):
            p = os.path.join(run, f)
            out[f] = {"sha256": hashlib.sha256(open(p, "rb").read()).hexdigest(), "size": os.path.getsize(p)}
    return out


def snap_old(run):
    out = {}
    for root, dirs, files in os.walk(run):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if f in SKIP_FILES:
                continue
            p = os.path.join(root, f)
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            out[os.path.relpath(p, run)] = {"sha256": h.hexdigest(), "size": os.path.getsize(p)}
    return out


def main(a):
    if len(a) == 4 and a[1] == "snap":
        json.dump(snap(a[2]), open(a[3], "w"), indent=1, sort_keys=True)
        print("snapshot: %d files" % len(json.load(open(a[3]))))
        return 0
    if len(a) == 4 and a[1] == "diff":
        b, c = json.load(open(a[2])), json.load(open(a[3]))
        bad = [("new", k) for k in c if k not in b] + [("gone", k) for k in b if k not in c] + \
              [("changed", k) for k in b if k in c and b[k]["sha256"] != c[k]["sha256"]]
        for kind, k in bad:
            print("%s: %s" % (kind, k))
        print("demo guard: %d files before, %d after, %s" % (len(b), len(c), "IDENTICAL" if not bad else "DIFFERENT"))
        return 0 if not bad else 1
    print(__doc__)
    return 2


sys.exit(main(sys.argv))

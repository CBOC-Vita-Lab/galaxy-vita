#!/usr/bin/env python3
"""Post-run check for qa_sp_untouched_whole_session: ONE sha256 for the single-player GameData.bin across every process of a pair folder
(before and after), equal to the fixture's. save_guard judges each process alone; a continuing process (HARNESS_CONTINUE) starts from the
saves/ of the one it continues, so a change made by process N and hidden by an equal 'before' in N+1 is caught by the first process's own
guard; this adds the missing link: the first 'before' IS the fixture, and nothing in the chain differs from it.
  fingerprint_chain.py <pair folder> [fixture saves dir, default /data/saves0/saves]"""
import hashlib, json, os, sys
SP = 'title/00010000/524d4750/data/GameData.bin'
pair = sys.argv[1]; fixture = sys.argv[2] if len(sys.argv) > 2 else '/data/saves0/saves'
want = hashlib.sha256(open(os.path.join(fixture, SP), 'rb').read()).hexdigest()
bad = 0; seen = 0
for name in sorted(os.listdir(pair)):
    g = os.path.join(pair, name, 'save-guard.json')
    if not os.path.isfile(g):
        continue
    j = json.load(open(g)); seen += 1
    for k in ('before', 'after'):
        ok = j.get(k) == want
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name:8s} {k:6s} {(j.get(k) or '-')[:16]}")
print(f"fixture {want[:16]}: {seen} process(es), {'ALL SAME' if seen and not bad else 'DIFFERENT / none'}")
sys.exit(0 if seen and not bad else 1)

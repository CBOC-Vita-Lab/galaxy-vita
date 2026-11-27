#!/usr/bin/env python3
"""check_log.py: reads one console log.txt of a pack24 job and prints the facts the README's checks ask for; exit 0 = every check of the mode passed.

  python3 check_log.py <log.txt> [--mode refill|verify|info] [--key b4d4c2a9] [--entries N] [--allow-new 6]

  refill: key folder as expected; GXV_VITA_SEED=off said; RAM pack enabled=0 or absent; no compile/link failures; flow/tour done (if --entries N or a flow/tour ran);
          for a warm job (--warm): 'deferred shader warm-up finished'
  verify: key folder as expected; RAM pack enabled=1 loaded=1 source=package; no SEED=off; no failures; flow/tour done; 'linked by a draw in play' = 0;
          first-use 'programs linked by draws' = 0; 'neither' lookups at the title = 0; programs made by draws (stutter lines) <= --allow-new (the
          backend's own present programs are made at start-up, a few)
Counts are printed for both modes. Pure Python 3, no network.
"""
import re, sys, argparse

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("log"); ap.add_argument("--mode", default="info", choices=["refill", "verify", "info"])
    ap.add_argument("--key", default="b4d4c2a9"); ap.add_argument("--entries", type=int, default=0, help="entries the flow/tour should have done (0: whatever the log says)")
    ap.add_argument("--warm", action="store_true"); ap.add_argument("--allow-new", type=int, default=6)
    a = ap.parse_args()
    txt = open(a.log, errors="replace").read().splitlines()
    ok, rows = True, []
    def row(name, val, good=None):
        nonlocal ok
        rows.append(f"{'ok  ' if good else 'FAIL' if good is False else 'info'}  {name}: {val}")
        if good is False: ok = False
    def find(rx): return [m for m in (re.search(rx, l) for l in txt) if m]
    m = find(r"compiled-shader cache: \S*?([0-9a-f]{8}) \(key of")
    key = m[0].group(1) if m else None
    row("cache key folder", key or "(no line)", (key == a.key) if a.mode != "info" and key else None if a.mode == "info" else False)
    seedoff = bool(find(r"GXV_VITA_SEED=off"))
    row("GXV_VITA_SEED=off said", seedoff, (seedoff if a.mode == "refill" else not seedoff) if a.mode != "info" else None)
    m = find(r"shader RAM pack: enabled=(\d) loaded=(\d) records=(\d+).*?source=(\w+)")
    if m:
        en, ld, rec, src = m[0].groups(); row("RAM pack", f"enabled={en} loaded={ld} records={rec} source={src}",
            (en == "1" and ld == "1" and int(rec) > 0 and src == "package") if a.mode == "verify" else (en == "0") if a.mode == "refill" else None)
    else:
        row("RAM pack", "(no line)", False if a.mode == "verify" else None)
    fails = find(r"did not compile|link failed|compile failed|FATAL")
    row("compile/link failures", len(fails), (len(fails) == 0) if a.mode != "info" else None)
    wf = find(r"deferred shader warm-up finished \((\d+) units")
    row("deferred shader warm-up finished", wf[0].group(1) + " units" if wf else "absent", (bool(wf)) if a.warm else None)
    ents = find(r"flow: entry (\d+) of (\d+)"); done = find(r"flow: done: all (\d+) entries"); tj = find(r"tour: jumping to (\S+) scenario (\d+)")
    if ents or done or tj:
        n = int(ents[0].group(2)) if ents else len(tj)
        last = max((int(m.group(1)) for m in ents), default=0)
        good = bool(done) if ents else (len(tj) >= (a.entries or 1))
        row("flow", f"{last} of {n} entries reached, done line {'present' if done else 'absent'}; tour jumps {len(tj)}", good if a.mode != "info" else None)
        if a.entries and last and last < a.entries: row("entries", f"{last} < expected {a.entries}", False)
        if not done and ents:
            e = ents[-1]; row("resume", f"last 'flow: entry {e.group(1)} of {e.group(2)}': re-run the entries from there (make_jobs.py stops <name> \"...\" --flow)", None)
    sc = find(r"script: t = 0 reached"); row("script starts (scenes with the route)", len(sc))
    inplay = find(r"linked by a draw in play")
    row("'linked by a draw in play' lines", len(inplay), (len(inplay) == 0) if a.mode == "verify" else None)
    fu = [int(m.group(1)) for m in find(r"first-use: stage \S+ (?:left|periodic)[^;]*; programs linked by draws (\d+)")]
    row("first-use programs linked by draws (sum over stages)", f"{sum(fu)} in {len(fu)} reports", (sum(fu) == 0) if a.mode == "verify" else None)
    nv = find(r"draws' new programs (\d+) \(")
    new = sum(int(m.group(1)) for m in nv); row("programs made by draws (stutter lines)", f"{new} in {sum(1 for m in nv if int(m.group(1)))} frames", (new <= a.allow_new) if a.mode == "verify" else None)
    wl = [int(m.group(1)) for m in find(r"warm-up \d+ ms \((\d+) links\)")]; sl = [int(m.group(1)) for m in find(r"stage warm-up \d+ \([\d.]+ ms, (\d+) links\)")]
    row("links by the title warm-up / stage warm-ups (stutter lines, total)", f"{sum(wl)} / {sum(sl)}")
    nei = find(r"shader lookups so far: (\d+) from the package/card RAM pack, (\d+) from the user pack, (\d+) neither")
    if nei: row("lookups at the title", f"{nei[-1].group(1)} pack, {nei[-1].group(2)} user pack, {nei[-1].group(3)} neither", (int(nei[-1].group(3)) == 0) if a.mode == "verify" else None)
    pa = find(r"player-assets:"); pt = find(r"powerup-test:"); row("player-assets / powerup-test lines", f"{len(pa)} / {len(pt)}")
    print("\n".join(rows)); print("RESULT", "PASS" if ok else "FAIL"); return 0 if ok else 3

if __name__ == "__main__": sys.exit(main())

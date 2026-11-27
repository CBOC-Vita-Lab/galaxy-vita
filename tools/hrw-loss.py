#!/usr/bin/env python3
"""hrw-loss.py LOG.txt [...]: what a player sees as stutter in the re-projected 60 (Performance, GXV_HRW=5), from a console log.

stutter37 (15 Nov 2026). While pairs run every picture should stay one refresh; a picture that stays two is, almost always, a pair whose
SECOND picture was not made. This prints, per stage and for the run, over the 5 s "hrw:" windows in which every drawn turn opened a pair:

  seconds, pairs                              the play the figures are over
  held 2 / 3 / 4+                             pictures that stayed 2, 3, 4 or more refreshes ("present:" lines, by overlap with those windows)
  lost second pictures, by reason:
    cut      a camera cut, a pause or a wipe between the pair's turns          (by design)
    guard    the camera turned or moved more than the guard band in one turn   (GXV_HRW_ROT_DEG / GXV_HRW_TRANS)
    hud      "HUD rectangles": the HUD-edge rule (GXV_HRW_HUD_EDGE)             (and the HUD's other refusals, none seen since c158)
    strip    "edge strip too wide": GXV_HRW_EDGE_MAX_PX and its hysteresis
    queue    asked by the game, never reached the warp: the render queue was 3 jobs behind (the GPU or the display queue is the limit)
    other    any other reason of the "hrw: warp" line
  late     pictures whose hold differed from their step in game turns ("held-vs-step off by 1+"): late pictures, and drops absorbed by the queue

and, for the run: the hitch lines inside those windows by cause, the programs linked in play (GXV_HRW_PREWARM), the drawn turns without a pair
inside stages by reason, and the short settles taken (GXV_HRW_SETTLE_RESUME).

A/B: run it on both logs of the same route; the lines are made to be compared side by side.  Python 3, no dependencies.
"""
import collections
import re
import sys


def secs(stamp):
    h, m, r = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(r)


LINE = re.compile(r"(\d\d:\d\d:\d\d\.\d+) (.*)")
STAGE = re.compile(r"mode: \d+, stage (\S+?),")
MODE5 = re.compile(r"hrw: mode 5 \| pairs (\d+) \(no pair: (\d+), camera cuts (\d+), over the guard band (\d+), asked (\d+)\)")
WARP = re.compile(r"hrw: warp \| pairs \d+ .*?asked (\d+), warped B (\d+), fell back (\d+) by reason (.*?) \|")
PRESENT = re.compile(r"present: .*?held \S+ vblanks ([\d/]+); game steps \S+ turns ([\d/]+); held-vs-step off by \S+ ([\d/]+)")
HITCH = re.compile(r"hitch: frame \d+ (\d+) ms \(back end \d+\): (.*?) \d+ ms, then")
# (emufix31: the line now says "the stage's choice says no re-projection N (its row / GXV_HRW_STAGE r, the GXV_HRW_STAGE_AUTO fallback a)"; both wordings are read)
NOPAIR = re.compile(r"hrw: no pair on (\d+) drawn turns: stage not in play or not settled yet .*? (\d+), the stage.s (?:row|choice) says no re-projection "
                    r"(?:\(GXV_HRW_STAGE\) )?(\d+)(?: \(its row / GXV_HRW_STAGE \d+, the GXV_HRW_STAGE_AUTO fallback \d+\))?, no game camera \(demo, cut-scene, no scene\) (\d+), other (\d+)")
REASON = re.compile(r"([A-Za-z][A-Za-z_ /\-]*?) (\d+)(?:,|$)")


def read(path):
    stage = "?"
    wins, pres, hitches = [], [], []
    cur = None
    extra = collections.Counter()
    for raw in open(path, errors="replace"):
        m = LINE.match(raw)
        if not m:
            continue
        t, body = secs(m.group(1)), m.group(2)
        s = STAGE.match(body)
        if s:
            stage = s.group(1)
            continue
        s = MODE5.match(body)
        if s:
            g = [int(x) for x in s.groups()]
            cur = dict(t=t, stage=stage, pairs=g[0], nopair=g[1], cut=g[2], guard=g[3], asked=g[4], reached=0, fell=collections.Counter())
            wins.append(cur)
            continue
        s = WARP.match(body)
        if s and cur and abs(t - cur["t"]) < 0.2:
            cur["reached"] = int(s.group(1))
            for name, n in REASON.findall(s.group(4)):
                cur["fell"][name.strip()] += int(n)
            continue
        s = PRESENT.match(body)
        if s:
            pres.append((t, [int(x) for x in s.group(1).split("/")], [int(x) for x in s.group(3).split("/")]))
            continue
        s = HITCH.match(body)
        if s:
            hitches.append((t, int(s.group(1)), s.group(2)))
            continue
        s = NOPAIR.match(body)
        if s and stage not in ("?", "FileSelect:1"):
            extra["nopair: not in play / not settled"] += int(s.group(2))
            extra["nopair: the stage's choice (its row or the GXV_HRW_STAGE_AUTO fallback)"] += int(s.group(3))
            extra["nopair: no game camera"] += int(s.group(4))
            extra["nopair: other"] += int(s.group(5))
            continue
        if body.startswith("hrw: prewarm |") and "LINKED IN PLAY" in body:
            extra["programs linked in play"] += 1
            ms = re.search(r": ([\d.]+) ms on the render thread", body)
            if ms:
                extra["programs linked in play, ms"] += float(ms.group(1))
        elif body.startswith("hrw: settle |"):
            extra["short settles (GXV_HRW_SETTLE_RESUME)"] += 1
        elif body.startswith("hrw: HUD edge |"):
            extra["HUD edge lines"] += 1
    return wins, pres, hitches, extra


def table(path):
    wins, pres, hitches, extra = read(path)
    full = [w for w in wins if w["pairs"] >= 100 and w["nopair"] == 0]
    by = collections.OrderedDict()
    for w in full:
        c = by.setdefault(w["stage"], collections.Counter())
        c["secs"] += 5
        c["pairs"] += w["pairs"]
        c["cut"] += w["cut"]
        c["guard"] += w["guard"]
        c["queue"] += max(0, w["asked"] - w["reached"])
        for name, n in w["fell"].items():
            c["hud" if name == "HUD rectangles" else "strip" if name == "edge strip too wide" else "other"] += n
        for (pt, held, err) in pres:   # a "present:" window ends at pt and is 5 s long: its share inside this window
            ov = max(0.0, min(pt, w["t"]) - max(pt - 5.0, w["t"] - 5.0)) / 5.0
            if ov > 0:
                c["h2"] += held[2] * ov
                c["h3"] += held[3] * ov
                c["h4"] += held[4] * ov
                c["late"] += sum(err[1:]) * ov
    print("== %s" % path.split("/")[-1])
    print("  %-26s %5s %6s | %6s %5s %5s | %5s = %4s %5s %4s %5s %5s %5s | %5s" %
          ("stage (windows all in pairs)", "s", "pairs", "held 2", "3", "4+", "lost", "cut", "guard", "hud", "strip", "queue", "other", "late"))
    tot = collections.Counter()
    for stage, c in by.items():
        lost = c["cut"] + c["guard"] + c["hud"] + c["strip"] + c["queue"] + c["other"]
        print("  %-26s %5d %6d | %6.0f %5.0f %5.0f | %5d = %4d %5d %4d %5d %5d %5d | %5.0f" %
              (stage[:26], c["secs"], c["pairs"], c["h2"], c["h3"], c["h4"], lost, c["cut"], c["guard"], c["hud"], c["strip"], c["queue"], c["other"], c["late"]))
        tot.update(c)
    if tot["secs"]:
        lost = tot["cut"] + tot["guard"] + tot["hud"] + tot["strip"] + tot["queue"] + tot["other"]
        print("  %-26s %5d %6d | %6.0f %5.0f %5.0f | %5d = %4d %5d %4d %5d %5d %5d | %5.0f" %
              ("RUN", tot["secs"], tot["pairs"], tot["h2"], tot["h3"], tot["h4"], lost, tot["cut"], tot["guard"], tot["hud"], tot["strip"], tot["queue"], tot["other"], tot["late"]))
        print("  a second of pairs: held 2 refreshes %.2f, 3 or more %.2f; second pictures lost %.2f (%.1f %% of pairs): hud %.0f %%, queue %.0f %%, strip %.0f %%, guard %.0f %%, cut %.0f %%, other %.0f %%" %
              (tot["h2"] / tot["secs"], (tot["h3"] + tot["h4"]) / tot["secs"], lost / tot["secs"], 100.0 * lost / max(1, tot["pairs"]),
               *[100.0 * tot[k] / max(1, lost) for k in ("hud", "queue", "strip", "guard", "cut", "other")]))
    # the hitch lines by where they fell
    cls = collections.Counter()
    ms = collections.Counter()
    for (t, dur, cause) in hitches:
        w = next((w for w in wins if w["t"] - 5.05 <= t <= w["t"]), None)
        where = "no window" if w is None else "all pairs" if w["pairs"] >= 100 and w["nopair"] == 0 else "no pairs" if w["pairs"] == 0 else "pairs begin or end"
        cls[(where, cause[:44])] += 1
        ms[(where, cause[:44])] += dur
    print("  hitch lines (over 40 ms in the back end's view; not all are seen: the present queue hides those under its slack):")
    for key in sorted(cls):
        print("    %-18s %-44s %3d  %5d ms" % (key[0], key[1], cls[key], ms[key]))
    for key in sorted(extra):
        print("  %s: %s" % (key, ("%.1f" % extra[key]) if isinstance(extra[key], float) else extra[key]))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        table(p)

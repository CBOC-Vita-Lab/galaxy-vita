#!/usr/bin/env python3
"""Galaxy 3.0 multiplayer: pose smoothing measurements from SMG_MP_POSELOG=1 game logs (owner: netsmooth).

  poselog.py trace RUN_DIR_OR_LOG [-o TRACE] [--slot N]
      the "tx" lines (a player's own pose every turn) -> a trace for tests/mp/netsmooth_sim.cpp ("t x y z qx qy qz qw vx vy vz
      flags anim frame"), the longest stretch of one scene with the player visible and moving.
  poselog.py plot PAIR_DIR [PAIR_DIR ...] -o PNG [--from S] [--seconds S]
      the puppet's speed per picture (units/turn) against the sender's own at the same moment, one panel per pair run:
      a smooth puppet follows the sender's curve; a hitch is a spike or a drop to zero (matplotlib).
  poselog.py judge PAIR_DIR --sender LABEL --receiver LABEL [--slot N] [--csv] [--seconds N] [--skip-hitches]
      what the receiver's puppet showed ("rx" lines) against the sender's own motion ("tx" lines), on the shared monotonic clock
      (both processes run on one machine): the same metrics as the simulator (err, delay, jerk/kinks, snaps, hold, extrap, anim).

The lines (decomp/src/Game/MP/MpPuppets.cpp):
  [mp] poselog tx slot=S t=TURN now=TURN ms=MS st=0.00 r=SENT p=x,y,z q=x,y,z,w v=x,y,z f=FLAGS a=ANIM fr=FRAME
  [mp] poselog in slot=S t=SENDER_TURN now=LOCAL_TURN ms=MS ...                  (a STATE arrived)
  [mp] poselog rx slot=S t=NEWEST now=LOCAL_TURN ms=MS st=REL r=RESULT p=... fr=  (shown; rendered sender turn = t + st)
"""
import argparse
import bisect
import math
import os
import re
import sys

LINE = re.compile(r"\[mp\] poselog (tx|in|rx) slot=(-?\d+) t=(\d+) now=(\d+) ms=(\d+) st=(-?[\d.]+) r=(\d+) "
                  r"p=([-\d.e+]+),([-\d.e+]+),([-\d.e+]+) q=([-\d.e+]+),([-\d.e+]+),([-\d.e+]+),([-\d.e+]+) "
                  r"v=([-\d.e+]+),([-\d.e+]+),([-\d.e+]+) f=([0-9a-f]+) a=([0-9a-f]+) fr=([-\d.e+]+)")
TURN_MS = 1000.0 / 60.0


def find_log(path):
    if os.path.isdir(path):
        for name in ("console.log", "game.log"):
            p = os.path.join(path, name)
            if os.path.exists(p):
                return p
    return path


def read(path):
    out = {"tx": [], "in": [], "rx": []}
    seen = set()
    with open(find_log(path), errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            g = m.groups()
            key = (g[0], g[1], g[3], g[2])   # (game.log and console.log may both carry a line: keep one)
            if key in seen:
                continue
            seen.add(key)
            rec = {
                "kind": g[0], "slot": int(g[1]), "t": int(g[2]), "now": int(g[3]), "ms": int(g[4]), "st": float(g[5]),
                "r": int(g[6]), "p": tuple(float(x) for x in g[7:10]), "q": tuple(float(x) for x in g[10:14]),
                "v": tuple(float(x) for x in g[14:17]), "f": int(g[17], 16), "a": int(g[18], 16), "fr": float(g[19]),
            }
            out[g[0]].append(rec)
    return out


def cmd_trace(a):
    d = read(a.run)
    tx = [r for r in d["tx"] if a.slot < 0 or r["slot"] == a.slot]
    if not tx:
        sys.exit("no tx lines (SMG_MP_POSELOG=1?)")
    # contiguous stretches (a scene change restarts nothing in the turn counter, but a jump > 2500 units is a warp / new scene)
    runs, cur = [], [tx[0]]
    for r in tx[1:]:
        p = cur[-1]
        jump = math.dist(r["p"], p["p"])
        if r["t"] != p["t"] + 1 or jump > 2500 or not (r["f"] & 1):
            if len(cur) > 1:
                runs.append(cur)
            cur = [r]
        else:
            cur.append(r)
    runs.append(cur)

    def motion(run):
        return sum(math.dist(run[i]["p"], run[i - 1]["p"]) for i in range(1, len(run)))
    best = max(runs, key=motion)
    out = open(a.output, "w") if a.output else sys.stdout
    out.write("# from %s: turns %d..%d, %.0f units moved\n" % (a.run, best[0]["t"], best[-1]["t"], motion(best)))
    for r in best:
        out.write("%d %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %04x %08x %.3f\n" % (
            r["t"], *r["p"], *r["q"], *r["v"], r["f"], r["a"], r["fr"]))
    if a.output:
        out.close()
        print("trace: %d turns (%.0f s), %.0f units moved, ground %.0f%% -> %s" % (
            len(best), len(best) / 60.0, motion(best), 100.0 * sum(1 for r in best if r["f"] & 2) / len(best), a.output))


def pct(v, q):
    if not v:
        return 0.0
    s = sorted(v)
    return s[int(q * (len(s) - 1))]


def cmd_judge(a):
    snd = read(os.path.join(a.pair, a.sender))
    rcv = read(os.path.join(a.pair, a.receiver))
    tx = sorted((r for r in snd["tx"]), key=lambda r: r["t"])
    rx = [r for r in rcv["rx"] if r["slot"] == a.slot]
    arrivals = [r for r in rcv["in"] if r["slot"] == a.slot]
    if rx and a.seconds > 0:   # only the first N seconds of the puppet (e.g. to compare with a shorter run)
        rx = [r for r in rx if r["ms"] - rx[0]["ms"] <= a.seconds * 1000]
        arrivals = [r for r in arrivals if r["ms"] <= rx[-1]["ms"]]
    if not tx or not rx:
        sys.exit("no tx (%d) or rx (%d) lines" % (len(tx), len(rx)))
    by_t = {r["t"]: r for r in tx}
    tx_t = [r["t"] for r in tx]
    tx_ms = [r["ms"] for r in tx]

    def truth(st):
        i = int(math.floor(st))
        a0, b0 = by_t.get(i), by_t.get(i + 1)
        if a0 is None:
            return None
        if b0 is None or math.dist(a0["p"], b0["p"]) > 2500:
            return a0["p"], a0["a"], a0["fr"]
        k = st - i
        return tuple(a0["p"][j] + (b0["p"][j] - a0["p"][j]) * k for j in range(3)), a0["a"], a0["fr"] + (b0["fr"] - a0["fr"]) * k

    def sender_turn_at(ms):
        i = bisect.bisect_left(tx_ms, ms)
        if i <= 0 or i >= len(tx_ms):
            return None
        m0, m1 = tx_ms[i - 1], tx_ms[i]
        k = 0.0 if m1 == m0 else (ms - m0) / (m1 - m0)
        return tx_t[i - 1] + (tx_t[i] - tx_t[i - 1]) * k

    # the games' own hitches (> 100 ms without a turn) on each side; --skip-hitches leaves out the 30 turns after one of the
    # receiver's (its whole picture jumped, not only the puppet)
    rgaps = [rx[i]["ms"] - rx[i - 1]["ms"] for i in range(1, len(rx))]
    tgaps = [tx[i]["ms"] - tx[i - 1]["ms"] for i in range(1, len(tx))]
    hitch_r = sum(1 for g in rgaps if g > 100)
    hitch_s = sum(1 for g in tgaps if g > 100 and g < 2000)
    after_hitch = set()
    for i, g in enumerate(rgaps):
        if g > 50:
            after_hitch.update(range(i + 1, i + 31))
    skipped = 0
    err, delay, jerk = [], [], []
    kinks = snaps = holds = extraps = anim = n = warps = 0
    hist = []   # (p, truth p, frame, truth frame, anim, truth anim, now)
    moving = 0
    for idx, r in enumerate(rx):
        if a.skip_hitches and idx in after_hitch:
            skipped += 1
            hist = []
            continue
        st = r["t"] + r["st"]
        tr = truth(st)
        sn = sender_turn_at(r["ms"])
        if tr is None or sn is None:
            hist = []
            continue
        tp, ta, tf = tr
        if hist and r["now"] != hist[-1][6] + 1:
            hist = []
        if hist and math.dist(tp, hist[-1][1]) > 500.0:
            hist = []   # the sender warped (a pipe, a launch, a new scene): not a smoothing matter
        if math.dist(r["p"], tp) > 2000.0:
            warps += 1   # (the puppet still shows the place before a warp: counted apart)
            hist = []
            continue
        n += 1
        err.append(math.dist(r["p"], tp))
        delay.append((sn - st) * TURN_MS)
        holds += r["r"] == 3
        extraps += r["r"] == 2
        if hist:
            moving += math.dist(tp, hist[-1][1]) > 1.0
        if len(hist) >= 2:
            (p1, t1, *_), (p2, t2, *_) = hist[-1], hist[-2]
            j2 = [(r["p"][k] - 2 * p1[k] + p2[k]) - (tp[k] - 2 * t1[k] + t2[k]) for k in range(3)]
            jj = math.sqrt(sum(x * x for x in j2))
            jerk.append(jj)
            kinks += jj > 20.0
            s1 = [(r["p"][k] - p1[k]) - (tp[k] - t1[k]) for k in range(3)]
            snaps += math.sqrt(sum(x * x for x in s1)) > 25.0
        if hist:
            _, _, f1, tf1, a1, ta1, _ = hist[-1]
            if a1 == r["a"] and ta1 == ta and ta == r["a"]:
                dd, dt = r["fr"] - f1, tf - tf1
                if abs(dt) < 5 and abs(dd) < 5 and abs(dd - dt) > 1.5:
                    anim += 1
        hist.append((r["p"], tp, r["fr"], tf, r["a"], ta, r["now"]))
        hist = hist[-3:]
    late = []
    if arrivals:
        # one-way delay of each STATE (arrival ms - the sender's ms at that turn; one machine, one monotonic clock), and its
        # lateness against the fastest one
        owd = [x["ms"] - by_t[x["t"]]["ms"] for x in arrivals if x["t"] in by_t]
        if owd:
            best = min(owd)
            late = [o - best for o in owd]
    per_min = 3600.0 / max(1, n)
    row = dict(turns=n, moving=100.0 * moving / max(1, n), err_mean=sum(err) / max(1, len(err)), err_p95=pct(err, 0.95),
               err_p999=pct(err, 0.999), delay_mean=sum(delay) / max(1, len(delay)), delay_p95=pct(delay, 0.95),
               jerk_mean=sum(jerk) / max(1, len(jerk)), jerk_p999=pct(jerk, 0.999), bumps_min=kinks * per_min,
               snaps_min=snaps * per_min, hold_pct=100.0 * holds / max(1, n), extrap_pct=100.0 * extraps / max(1, n),
               anim_min=anim * per_min, states=len(arrivals), late_p95=pct(late, 0.95), late_max=max(late) if late else 0,
               warp_turns=warps, hitches_receiver=hitch_r, hitches_sender=hitch_s, skipped=skipped)
    if a.csv:
        print(",".join(row.keys()))
        print(",".join("%.2f" % v for v in row.values()))
    else:
        print("%s: %d turns shown (%.0f%% with the sender moving; %d turns across a warp left out%s), %d STATEs received, one-way "
              "delay spread p95 %.0f ms max %.0f ms; game hitches > 100 ms: receiver %d, sender %d" % (
                  a.pair, n, row["moving"], warps, (", %d after a receiver hitch" % skipped) if a.skip_hitches else "", len(arrivals),
                  row["late_p95"], row["late_max"], hitch_r, hitch_s))
        print("  err mean %.2f p95 %.2f p99.9 %.1f | delay mean %.0f p95 %.0f ms | jerk mean %.2f p99.9 %.1f | bumps/min %.1f "
              "snaps/min %.1f | hold %.2f%% extrap %.2f%% | anim hiccups/min %.1f" % (
                  row["err_mean"], row["err_p95"], row["err_p999"], row["delay_mean"], row["delay_p95"], row["jerk_mean"],
                  row["jerk_p999"], row["bumps_min"], row["snaps_min"], row["hold_pct"], row["extrap_pct"], row["anim_min"]))


def speed_series(pair, sender, receiver, slot):
    snd = read(os.path.join(pair, sender))
    rcv = read(os.path.join(pair, receiver))
    by_t = {r["t"]: r for r in snd["tx"]}
    rx = [r for r in rcv["rx"] if r["slot"] == slot]
    out = []   # (seconds since the first rx, shown speed, truth speed at the rendered turn, result)
    prev = None
    for r in rx:
        st = r["t"] + r["st"]
        i = int(math.floor(st))
        a0, b0 = by_t.get(i), by_t.get(i + 1)
        if a0 is None or b0 is None:
            prev = None
            continue
        k = st - i
        tp = tuple(a0["p"][j] + (b0["p"][j] - a0["p"][j]) * k for j in range(3))
        if prev is not None and r["now"] == prev[0] + 1:
            out.append(((r["ms"] - rx[0]["ms"]) / 1000.0, math.dist(r["p"], prev[1]), math.dist(tp, prev[2]), r["r"]))
        prev = (r["now"], r["p"], tp)
    return out


def cmd_plot(a):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(len(a.pairs), 1, figsize=(12, 3.2 * len(a.pairs)), sharex=True, squeeze=False)
    for ax, pair in zip(axes[:, 0], a.pairs):
        ser = [x for x in speed_series(pair, a.sender, a.receiver, a.slot) if a.start <= x[0] <= a.start + a.seconds]
        t = [x[0] for x in ser]
        ax.plot(t, [x[2] for x in ser], color="#9aa5b1", lw=2.2, label="sender (truth at the rendered moment)")
        ax.plot(t, [x[1] for x in ser], color="#1f6feb", lw=1.0, label="puppet as shown")
        hold = [x[0] for x in ser if x[3] == 3]
        if hold:
            ax.plot(hold, [0] * len(hold), "|", color="#d1242f", ms=8, label="held")
        ax.set_title(os.path.basename(pair.rstrip("/")), loc="left", fontsize=10)
        ax.set_ylabel("units / turn")
        ax.set_ylim(0, max(40, max([x[2] for x in ser] or [0]) * 1.3))
        ax.legend(loc="upper right", fontsize=8, frameon=False)
    axes[-1, 0].set_xlabel("seconds (receiver)")
    fig.tight_layout()
    fig.savefig(a.output, dpi=110)
    print("plot -> %s" % a.output)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trace")
    t.add_argument("run")
    t.add_argument("-o", "--output")
    t.add_argument("--slot", type=int, default=-1)
    j = sub.add_parser("judge")
    j.add_argument("pair")
    j.add_argument("--sender", default="host")
    j.add_argument("--receiver", default="guest")
    j.add_argument("--slot", type=int, default=0)
    j.add_argument("--csv", action="store_true")
    j.add_argument("--seconds", type=float, default=0.0, help="only the first N seconds of the puppet")
    j.add_argument("--skip-hitches", action="store_true", help="leave out the 30 turns after each hitch of the receiving game")
    pl = sub.add_parser("plot")
    pl.add_argument("pairs", nargs="+")
    pl.add_argument("-o", "--output", required=True)
    pl.add_argument("--from", dest="start", type=float, default=0.0)
    pl.add_argument("--seconds", type=float, default=12.0)
    pl.add_argument("--sender", default="host")
    pl.add_argument("--receiver", default="guest")
    pl.add_argument("--slot", type=int, default=0)
    a = ap.parse_args()
    {"trace": cmd_trace, "judge": cmd_judge, "plot": cmd_plot}[a.cmd](a)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""far-est.py (far30 rev 2, q30far): the estimate tables of the far field. ESTIMATES, not measurements; every input is labelled.
Inputs: STAGE-SIZES.tsv lanes at 960x544 (game drawn turn, front end, GPU TA, GPU busy) and the in-play census of build c93 (c93-cenA: dist-last.py).
Bands: 0-25 | 25-50 | 50-100 | 100+ m of share of (logic, posed, draws, indices). Honeyhive has the full table and the class split; Good Egg and Dreadnought have the
cumulative shares the coordinator quoted (turned into bands here); Beach Bowl, Freezeflame, Space Junk have NONE yet (c93-cenB): a Honeyhive-shaped PROXY is used and flagged.
Unit costs (read from the Quality tour's breakdown, Honeyhive c90): draw 16 us + display-list/view flush 6.4 us per draw; pose 89 us per posed actor (flush 49 + calc 40); logic 35 us
per actor-turn; front end 13 us per draw; GPU TA 19.3 ns per index. 3D fill is not credited. Tiers act in the order rate, LOD, density (plants by per-group hash), draw (tiny only)."""
import sys
sys.path.insert(0, '.')
# share by band: (logic, posed, draws, indices) as fractions of the stage's totals
HONEY = {"tot": (234, 80, 551, 463e3),
         "b": [(0.13, 0.14, 0.10, 0.06), (0.086, 0.093, 0.021, 0.015), (0.394, 0.373, 0.518, 0.638), (0.39, 0.40, 0.36, 0.28)],
         "plants": [(0.0, 0.0), (0.0, 0.0), (115.4, 106024), (122.7, 48657)]}     # plants class: draws, indices in the 50-100 and 100+ bands
EGG = {"tot": (150, 83, 694, 390e3),
       "b": [(0.41, 0.42, 0.13, 0.09), (0.43, 0.35, 0.10, 0.20), (0.01, 0.02, 0.04, 0.02), (0.15, 0.21, 0.73, 0.69)],
       "plants": [(0, 0), (0, 0), (0.55 * 0.04 * 694, 0.55 * 0.02 * 390e3), (386.0, 167e3)]}   # 386 draws / 167k indices beyond 100 m: coordinator's census
DREAD = {"tot": (158, 85, 371, 372e3),
         "b": [(0.01, 0.01, 0.02, 0.01), (0.44, 0.51, 0.29, 0.28), (0.42, 0.33, 0.15, 0.08), (0.13, 0.15, 0.54, 0.63)],
         "plants": [(0, 0), (0, 0), (0.15 * 0.15 * 371, 0.15 * 0.08 * 372e3), (0.15 * 0.54 * 371, 0.15 * 0.63 * 372e3)]}   # plants share unknown: 15 % PROXY
S = {  # name: (game ms, front ms, gpu TA ms, gpu busy ms, census, quality of the census)
 "Honeyhive":   (22.5, 21.6, 13.1, 20.0, HONEY, "census c93"),
 "Beach Bowl":  (22.2, 21.3, 15.1, 24.9, HONEY, "PROXY (Honeyhive shape)"),
 "Freezeflame": (10.3,  9.6,  7.7, 22.8, HONEY, "PROXY (Honeyhive shape)"),
 "Dreadnought": (23.2, 15.2,  9.7, 21.9, DREAD, "census c93 (cumulative shares; plants share guessed at 15 %)"),
 "Space Junk":  (16.6, 16.1, 11.9, 19.1, HONEY, "PROXY (Honeyhive shape)"),
 "Good Egg":    (17.4, 14.5, 12.1, 18.6, EGG, "census c93 (cumulative shares)"),
}
EDGE = [25, 50, 100, 400]
PLANTS_GUESS = {"Beach Bowl": 0.7, "Freezeflame": 0.2, "Space Junk": 0.3}   # share of Honeyhive's plants class: a GUESS until c93-cenB
def beyond(c, h, k):
    """share of column k (0 logic, 1 posed, 2 draws, 3 indices) beyond h metres (uniform inside a band)"""
    t, lo = 0.0, 0.0
    for i, hi in enumerate(EDGE):
        sh = c["b"][i][k]
        if hi > h:
            t += sh * (hi - max(lo, h)) / (hi - lo)
        lo = hi
    return t
def plants_beyond(c, h):
    d = i = 0.0
    lo = 0.0
    for j, hi in enumerate(EDGE):
        pd, pi = c["plants"][j]
        if hi > h:
            f = (hi - max(lo, h)) / (hi - lo)
            d += pd * f; i += pi * f
        lo = hi
    return d, i
# horizons (m) per depth: rate, lod scale, density, draw (farfield.h Plan) ; k poses
DEPTH = {1: (48, 0.75, None, None, 2), 2: (38, 0.6, 60, None, 3), 3: (25, 0.5, 45, 90, 3), 4: (25, 0.4, 35, 50, 4)}
def run(perf=True):
    print("PERFORMANCE: per turn, limit 16.7 ms at 60 (games thread / front end / GPU busy), by far-field level 0..4 (4 = last resort)")
    print("%-12s %5s | %5s %6s %5s | %6s %6s %6s | %s" % ("stage", "level", "draws", "idx k", "posed", "game", "front", "gpu", "verdict"))
    for name, (g, f, ta, busy, c, src) in S.items():
        lg, ps, dr, ix = c["tot"]
        if "PROXY" in src:   # a proxy census is scaled to the stage's own game-thread load, and its plants share is a guess per stage (ice and space have few plants)
            k0 = g / 22.5
            lg, ps, dr, ix = lg * k0, ps * k0, dr * k0, ix * k0
            ps_ = PLANTS_GUESS[name]
            c = dict(c, plants=[(a * k0 * ps_, b * k0 * ps_) for a, b in c["plants"]])
        implied = lg * 35e-3 + dr * 22e-3 + ps * 89e-3        # what the census (budget OFF) would cost the game thread
        sg = min(1.0, g / implied)                            # the console lane already has the budget's cuts: savings are scaled to what is left
        sp = min(1.0, (ta - 1.0) / (ix * 19.3e-6))
        for lv in (0, 1, 2, 3, 4):
            if lv == 0:
                G, F, P, rd, ri, rp = g, f, busy, dr, ix, ps
            else:
                hr, ls, hd, hw, k = DEPTH[lv]
                poseSave = ps * beyond(c, hr, 1) * (1 - 1.0 / k)
                lodIdx = ix * beyond(c, 50, 3) * (1 - ls) * 0.30        # a third of the far models have a lower model; it saves ~ the scale's reduction
                dd = di = 0.0
                if hd:
                    dd, di = plants_beyond(c, hd)
                tiny = 0.0
                if hw:
                    tiny = 0.05 * (dr * beyond(c, hw, 2) - dd)           # draw tier: only what is under 12 px: ~5 % of the far draws (census: none under 8 px)
                    tiny = max(tiny, 0.0)
                rd = dr - dd - tiny
                ri = ix - di - lodIdx - tiny * (ix / dr) * 0.5
                rp = ps - poseSave
                G = g - sg * ((dd + tiny) * 22e-3 + poseSave * 89e-3)
                F = f - sg * (dd + tiny) * 13e-3
                P = busy - sp * (ta - 1.0) * (1 - ri / ix)
            print("%-12s %5d | %5.0f %6.0f %5.0f | %6.1f %6.1f %6.1f | %s" % (name, lv, rd, ri / 1e3, rp, G, F, P, "holds 60" if max(G, F, P) <= 16.7 else "no (%.1f)" % max(G, F, P)))
        print("   basis: %s" % src)
if __name__ == "__main__":
    run()

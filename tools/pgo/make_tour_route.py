#!/usr/bin/env python3
"""Write routes/tour.txt: the menu path of the opening route (new file, Play, storybook skipped), then the debug stage
tour (PETARI_STAGE list, PETARI_STAGE_HOLD frames of gameplay in each) with a pad script that wanders: stick forwards
with turns, star-spin, jump, pointer sweeps with B (star-bit shots) and A taps for speech boxes.

    python3 -I tools/pgo/make_tour_route.py [hold-seconds] > tools/pgo/routes/tour.txt

Times are game seconds (the harness runs on a virtual clock).  The stage list is the one the recorded profile used."""
import sys

STAGES = ["AstroGalaxy:1", "EggStarGalaxy:1", "EggStarGalaxy:2", "HeavenlyBeachGalaxy:1", "KoopaBattleVs3Galaxy:1"]
hold = int(sys.argv[1]) if len(sys.argv) > 1 else 45

out = ["# PGO tour route (tools/pgo/make_tour_route.py): menu path of opening.txt, storybook skipped by a hold of A,",
       "# then each galaxy of PETARI_STAGE for %d s of gameplay with a wandering pad script." % hold,
       "#!env PETARI_STAGE=" + ",".join(STAGES), "#!env PETARI_STAGE_HOLD=%d" % (hold * 60),
       "27-27.4:A|B", "31-36.8:PX=-0.25|PY=0.30", "34-34.2:A", "37-44.5:PX=0.29|PY=0.61", "40-40.2:A",
       "45-51.5:PX=-0.56|PY=-0.40", "48-48.2:A", "52-61.8:PX=0.29|PY=0.60", "55-55.2:A", "62-66:PX=0.52|PY=0.78",
       "65-65.2:A", "70-73:A"]
t0 = 76.0
end = t0 + 20 + len(STAGES) * (hold + 25)
t = t0
n = 0
while t < end:
    c = n % 4
    out.append("%.1f-%.1f:SY=1" % (t, t + 3.5))
    out.append("%.1f-%.1f:SY=1|SX=%.1f" % (t + 3.5, t + 6.5, 0.7 if c % 2 == 0 else -0.7))
    out.append("%.1f-%.1f:SY=1" % (t + 6.5, t + 9))
    out.append("%.1f-%.1f:SPIN" % (t + 2.0, t + 2.6))
    out.append("%.1f-%.1f:A" % (t + 4.0, t + 4.2))
    out.append("%.1f-%.1f:A" % (t + 7.5, t + 7.7))
    out.append("%.1f-%.1f:PX=%.1f|PY=%.1f" % (t + 1, t + 4, 0.5 - 0.2 * c, -0.3 + 0.2 * c))
    out.append("%.1f-%.1f:B" % (t + 1.5, t + 1.7))
    out.append("%.1f-%.1f:B" % (t + 3.0, t + 3.2))
    if n % 2 == 1:
        out.append("%.1f-%.1f:SY=-1|SX=0.5" % (t + 9, t + 10.5))
        t += 1.5
    t += 9.0
    n += 1
print("\n".join(out))

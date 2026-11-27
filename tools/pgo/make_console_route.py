#!/usr/bin/env python3
"""Write the pad script of the console recording (jobs/input-wander.txt -> ux0:data/smg/input.txt) in the format of the Vita port's input loader
(agents/route24/FORMAT.md: "<t0>-<t1>:item|item", seconds, one entry a line, comments whole lines, times spelled so that the loader's
(int)(atof(text) * 1000.0) reads exactly the millisecond meant).

The pattern is make_tour_route.py's wandering of the harness tours: blocks of 9 s of stick forwards with turns, a star-spin, jumps, pointer sweeps
with B (star-bit shots) and A taps for speech boxes; every second block ends with a short run backwards.  It runs with PETARI_SCRIPT_ANCHOR=play
(t = 0 at the first turn of play of every stage: the script starts again in each) and PETARI_SCRIPT_LOOP=<period> so that it repeats for as long
as the stage is held (PETARI_FLOW_HOLD).  The period is the length of the pattern (the last line below the header says it).

    python3 -I tools/pgo/make_console_route.py > jobs/input-wander.txt
"""
import sys

BLOCKS = 4  # 4 blocks = 39 s


def spell(ms):
    """The shortest decimal text (seconds) that the loader reads back as exactly `ms` milliseconds."""
    for digits in range(0, 4):
        text = "%.*f" % (digits, ms / 1000.0)
        if int(float(text) * 1000.0) == ms:
            return text
    text = "%.4f" % ((ms + 0.5) / 1000.0)  # (the middle of the millisecond)
    assert int(float(text) * 1000.0) == ms, ms
    return text


def entry(t0, t1, item):
    a, b = int(round(t0 * 1000)), int(round(t1 * 1000))
    return "%s-%s:%s" % (spell(a), spell(b), item)


def main():
    out, t = [], 0.0
    for n in range(BLOCKS):
        c = n % 4
        out.append(entry(t, t + 3.5, "SY=1"))
        out.append(entry(t + 3.5, t + 6.5, "SY=1|SX=%.1f" % (0.7 if c % 2 == 0 else -0.7)))
        out.append(entry(t + 6.5, t + 9, "SY=1"))
        out.append(entry(t + 2.0, t + 2.6, "SPIN"))
        out.append(entry(t + 4.0, t + 4.2, "A"))
        out.append(entry(t + 7.5, t + 7.7, "A"))
        out.append(entry(t + 1, t + 4, "PX=%.1f|PY=%.1f" % (0.5 - 0.2 * c, -0.3 + 0.2 * c)))
        out.append(entry(t + 1.5, t + 1.7, "B"))
        out.append(entry(t + 3.0, t + 3.2, "B"))
        if n % 2 == 1:
            out.append(entry(t + 9, t + 10.5, "SY=-1|SX=0.5"))
            t += 1.5
        t += 9.0
    head = ["# Wandering pad script of the console PGO recording (tools/pgo/make_console_route.py): %d entries, one pattern of %.1f s." % (len(out), t),
            "# Use with PETARI_SCRIPT_ANCHOR=play and PETARI_SCRIPT_LOOP=%d (the pattern repeats; it starts again in every stage)." % int(t + 0.5),
            "# Format and clocks: agents/route24/FORMAT.md. Never PLUS or MINUS (they pause)."]
    print("\n".join(head + out))


if __name__ == "__main__":
    main()

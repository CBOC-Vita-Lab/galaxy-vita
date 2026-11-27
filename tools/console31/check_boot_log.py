#!/usr/bin/env python3
"""check_boot_log.py: the start-up facts of a console log.txt that every session must read first (strk31).

  python3 -I tools/console31/check_boot_log.py LOG [LOG...]      exit 0 = every log has the clock lines, 1 = a line is missing, 2 = no such file

Clocks (vita/vgx-game/entry.cpp). The game asks for CPU 500 MHz (444 if refused), bus 222, GPU 222, crossbar 166 and logs the request and the result:
  clocks: CPU request=500 arm_rc=0x........ fallback444 ... bus_rc=... gpu_rc=... xbar_rc=...; reported CPU=.. bus=.. GPU=.. xbar=..    (at start)
  clocks: 1 s into the process the ARM core measures N MHz (asked for 500 | a profile is in place: no request; arm_rc=0x........ fallback_rc=0x........)  (GXV_CLOCK_DEFER=1, the default: the CPU request is made here)
  clocks: the ARM core measures N MHz before the requests and M MHz after them
A request the system refuses shows as rc 0x802B0000 (Strikers' 500 MHz request is refused on every run; the game then falls back to 444). A refused 500 is a FACT to
write down (the run's CPU clock is then 444, or the plugin profile's), not a failure of the log. A missing line is the failure: the run's clock is unknown.
"""
import re, sys

REFUSED = 0x802B0000


def facts(text):
    """-> (problems, notes) for one log's text"""
    problems, notes = [], []
    lines = [l for l in text.splitlines() if "clocks:" in l]
    if not lines:
        return ["no 'clocks:' line at all: the CPU/GPU clock request and its result are not in this log"], notes
    start = [l for l in lines if re.search(r"clocks: (CPU request=|requests disabled)", l)]
    if not start:
        problems.append("no start line 'clocks: CPU request=... arm_rc=...' (or 'requests disabled')")
    disabled = any("requests disabled" in l for l in start)
    if disabled:
        notes.append("clock requests are disabled (GXV_VITA_CLOCKS=0): the game asked for nothing")
    for l in start:
        m = re.search(r"CPU request=(\d+) arm_rc=0x([0-9A-Fa-f]{8}).*?bus_rc=0x([0-9A-Fa-f]{8}).*?gpu_rc=0x([0-9A-Fa-f]{8}).*?xbar request=(\d+) xbar_rc=0x([0-9A-Fa-f]{8})", l)
        if m:
            for name, v in (("bus", m.group(3)), ("GPU", m.group(4)), ("crossbar", m.group(6))):
                rc = int(v, 16)
                if rc & 0x80000000:
                    notes.append(f"{name} clock request refused: rc=0x{rc:08X}")
            rep = re.search(r"reported CPU=(-?\d+) reported bus=(-?\d+) reported GPU=(-?\d+) reported xbar=(-?\d+)", l)
            if rep:
                notes.append("reported at start: CPU %s, bus %s, GPU %s, xbar %s MHz" % rep.groups())
    meas = [l for l in lines if "the ARM core measures" in l and "before the requests" in l]
    if not meas:
        problems.append("no 'clocks: the ARM core measures N MHz before the requests and M MHz after them' line")
    for l in meas:
        m = re.search(r"measures (\d+) MHz before the requests and (\d+) MHz after", l)
        if m: notes.append(f"the ARM core measures {m.group(1)} MHz before and {m.group(2)} MHz after the requests (496-500: 500 MHz; 440-444: the 444 fallback)")
    deferred = [l for l in lines if "1 s into the process" in l]
    stock = any("GXV_STOCK_SIM=1" in l for l in lines)
    if not deferred and not disabled and not stock:
        problems.append("no deferred-request line 'clocks: 1 s into the process ... arm_rc=... fallback_rc=...' (GXV_CLOCK_DEFER=1 is the default; with =0 the request is in the start line)")
    for l in deferred:
        m = re.search(r"measures (\d+) MHz \(([^;)]*)(?:; arm_rc=0x([0-9A-Fa-f]{8}) fallback_rc=0x([0-9A-Fa-f]{8}))?\); now (\d+) MHz", l)
        if not m:
            problems.append("a 'clocks: 1 s into the process' line that does not parse: " + l.strip()[:120]); continue
        pre, what, arm, fb, now = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if arm is None: continue
        armrc, fbrc = int(arm, 16), int(fb, 16)
        if "no request" in what:
            notes.append(f"a profile held the core at {pre} MHz at 1 s: the game made no CPU request (now {now} MHz)")
        elif armrc == 0:
            notes.append(f"CPU 500 MHz request accepted (rc 0); the core measures {now} MHz")
        elif armrc == REFUSED:
            notes.append(f"CPU 500 MHz request REFUSED (rc 0x802B0000); fallback 444 rc=0x{fbrc:08X}; the core measures {now} MHz")
        else:
            notes.append(f"CPU 500 MHz request failed rc=0x{armrc:08X}; fallback 444 rc=0x{fbrc:08X}; the core measures {now} MHz")
    return problems, notes


def main(argv):
    if not argv:
        print(__doc__); return 2
    rc = 0
    for p in argv:
        try:
            text = open(p, errors="replace").read()
        except OSError as e:
            print(f"{p}: cannot read ({e})"); rc = max(rc, 2); continue
        problems, notes = facts(text)
        for n in notes: print(f"{p}: note: {n}")
        for q in problems: print(f"{p}: FAIL: {q}")
        print(f"{p}: {'FAIL' if problems else 'ok'}: clocks request and result {'MISSING' if problems else 'logged'}")
        if problems: rc = max(rc, 1)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

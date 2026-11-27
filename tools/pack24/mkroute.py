#!/usr/bin/env python3
"""mkroute.py - compose the Vita port's scripted pad input (ux0:data/smg/input.txt) with turn-accurate timing.

The file grammar and the clock rules are in ../FORMAT.md.  In short: one entry per line,
"<t0>-<t1>:<item>|<item>...", t in seconds, active for t0 <= t < t1; the clock is 60 turns per second and the port reads
"ms = turn * 1000 / 60" (integer division).  This tool works in TURNS and writes, for each run of turns, the two times
ms(first turn) and ms(first turn after the run), in a spelling that the port's own parser (atof * 1000, truncated) reads back
EXACTLY (a plain "0.016" would read as 15 ms: the parser truncates 15.999...).  Every file it writes is read back by a strict
re-implementation of the port's loader + parser and replayed turn by turn against the intended inputs.

Python use:
    from mkroute import Route
    r = Route("demo")
    r.wait(30)
    r.run("F", 45)                         # stick forward (camera-relative), magnitude 1.0, 45 turns
    with r.par() as p:                     # things that happen at the same time
        p.run("R", 60)
        p.at(20).jump()
    r.spin()
    open("input.txt", "w").write(r.text())

Command line:
    mkroute.py build SPEC [-o FILE] [--stock]   SPEC is a segment file (below); '-' reads stdin; prints to stdout without -o
    mkroute.py check FILE [--stock]             strict validation + summary of an existing input.txt
    mkroute.py selftest                         the unit tests (needs only python3; uses cc for the real-parser test if present)

Segment file (one command per line, '#' comments, numbers are TURNS unless noted; 60 turns = 1 s):
    wait N
    run DIR N [MAG]            stick DIR at MAG (default 1.0).  walk = same with MAG 0.5 default
    stick SX SY N              raw stick (x right, y up)
    hold BTN[+BTN..] N         buttons held for N turns (A B Z C 1 2 UP DOWN LEFT RIGHT PLUS MINUS HOME)
    press BTN[+BTN..] [N]      same, N default 6
    jump [HOLD [SETTLE]]       A held HOLD turns (14), then SETTLE more turns of waiting (40)
    spin [N [SETTLE]]          SPIN (remote shake) N turns (6), then SETTLE (78: the game's spin interval is ~79 turns)
    longjump [DIR [RUNIN [AIR [SETTLE]]]]   run DIR RUNIN turns (45), then Z then A with the stick held, AIR turns more (30), SETTLE (50)
    backflip [SETTLE]          Z (crouch), then A while standing, SETTLE (50)
    groundpound [SETTLE]       A, then Z in the air, SETTLE (45)
    crouch N                   Z held N turns
    camleft | camright [SETTLE]   one 45-degree camera step (D-pad LEFT / RIGHT trigger), SETTLE (48)
    camorbit [STEPS [left|right [GAP]]]   STEPS (8) camera steps GAP turns apart (50): 8 steps = 360 degrees
    camreset [SETTLE]          C (camera behind Mario), SETTLE (48)
    fp LOOKTURNS [MAG]         first-person: UP, wait 40, look sweep (right, left, up, down...) for LOOKTURNS, DOWN, wait 40
    pause [TURNS]              PLUS, wait TURNS (100), PLUS, wait 60
    circle N [TURNS_PER_REV [MAG [cw|ccw]]]   the stick direction turns once round every TURNS_PER_REV (240) turns, N turns long
    wiggle DIRA DIRB PERIOD N [MAG]   alternate the stick between two directions every PERIOD turns for N turns
    point PX PY N              pointer at PX,PY (-1..1) for N turns        shoot   B for 6 turns
    box LEG [MAG [JUMPS]]      forward, right, back, left, LEG turns each (JUMPS=1: a jump during legs 1 and 3)
    par ... end                lines inside start together; "at K <command>" starts that command K turns into the block
    repeat K ... end
DIR is F B L R FL FR BL BR or an angle in degrees (0 = forward/up on the stick, 90 = right, clockwise).
"""
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile

TPS = 60                       # game turns per second
STOCK_ENTRIES, STOCK_CHARS = 1024, 16000    # entry.cpp g_script[1024] and joined[16000] at 56912ef
LINE_MAX = 500                 # entry.cpp reads lines with fgets(line, 512): a longer line is cut and the cut joins with a comma
# Wii button bits (platform/include/port/input_script.h kPortScriptButtons)
BUTTONS = {"LEFT": 0x0001, "RIGHT": 0x0002, "DOWN": 0x0004, "UP": 0x0008, "PLUS": 0x0010, "2": 0x0100, "1": 0x0200,
           "B": 0x0400, "A": 0x0800, "MINUS": 0x1000, "Z": 0x2000, "C": 0x4000, "HOME": 0x8000}
BUTTON_ORDER = ["LEFT", "RIGHT", "DOWN", "UP", "PLUS", "2", "1", "B", "A", "MINUS", "Z", "C", "HOME"]
DIRS = {"F": 0, "FR": 45, "R": 90, "BR": 135, "B": 180, "BL": 225, "L": 270, "FL": 315}


def ms_of(turn):
    """What the port computes for game turn `turn` (entry.cpp: turn * 1000 / 60, integer division)."""
    return turn * 1000 // TPS


def parses_exact(text, ms):
    """The port's parser: (int)(atof(text) * 1000.0)."""
    return int(float(text) * 1000.0) == ms


def fmt_ms(ms):
    """Shortest spelling of `ms` milliseconds that the port's parser reads back as exactly `ms`."""
    for d in range(0, 4):
        if ms % (10 ** (3 - d)) == 0:
            s = "%.*f" % (d, ms / 1000.0)
            if parses_exact(s, ms):
                return s
    s = "%.4f" % ((ms + 0.5) / 1000.0)      # the middle of the millisecond: no truncation trap
    if not parses_exact(s, ms):
        raise AssertionError("cannot spell %d ms" % ms)
    return s


def fmt_f(v):
    s = ("%.3f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def dir_angle(d):
    if isinstance(d, str):
        key = d.upper()
        if key in DIRS:
            return DIRS[key]
        return float(d)
    return float(d)


def btn_mask(spec):
    if isinstance(spec, int):
        return spec
    m = 0
    for name in str(spec).upper().replace(",", "+").split("+"):
        name = name.strip()
        if not name:
            continue
        if name not in BUTTONS:
            raise ValueError("unknown button %r (have %s)" % (name, " ".join(BUTTON_ORDER)))
        m |= BUTTONS[name]
    return m


class Timeline:
    """What the pad does on each turn: buttons, stick, spin, pointer."""

    def __init__(self):
        self.buttons = []
        self.stick = []
        self.spin = []
        self.pointer = []

    def grow(self, n):
        if n < 0:
            raise ValueError("negative length")
        while len(self.buttons) < n:
            self.buttons.append(0)
            self.stick.append(None)
            self.spin.append(False)
            self.pointer.append(None)

    def __len__(self):
        return len(self.buttons)

    def set_buttons(self, a, n, mask):
        if n < 0 or a < 0:
            raise ValueError("negative start or length")
        self.grow(a + n)
        for t in range(a, a + n):
            self.buttons[t] |= mask

    def set_stick(self, a, n, sx, sy):
        if n < 0 or a < 0:
            raise ValueError("negative start or length")
        self.grow(a + n)
        sx, sy = round(sx, 3), round(sy, 3)
        for t in range(a, a + n):
            self.stick[t] = (sx, sy)

    def set_spin(self, a, n):
        self.grow(a + n)
        for t in range(a, a + n):
            self.spin[t] = True

    def set_pointer(self, a, n, px, py):
        self.grow(a + n)
        for t in range(a, a + n):
            self.pointer[t] = (round(px, 3), round(py, 3))


class Seg:
    """The segment library.  Each method adds to the timeline at self.t and moves self.t on (sequential); a Par block
    gives each `at(k)` its own Seg so that their actions start together."""

    def __init__(self, tl, t):
        self.tl = tl
        self.t = t

    # ---- primitives
    def wait(self, n):
        if n < 0:
            raise ValueError("wait: negative length")
        self.tl.grow(self.t + n)
        self.t += n
        return self

    def stick(self, sx, sy, n):
        self.tl.set_stick(self.t, n, sx, sy)
        self.t += n
        return self

    def run(self, d, n, mag=1.0):
        a = math.radians(dir_angle(d))
        return self.stick(mag * math.sin(a), mag * math.cos(a), n)

    def walk(self, d, n, mag=0.5):
        return self.run(d, n, mag)

    def hold(self, btns, n):
        self.tl.set_buttons(self.t, n, btn_mask(btns))
        self.t += n
        return self

    def press(self, btns, n=6):
        return self.hold(btns, n)

    def pointer(self, px, py, n):
        self.tl.set_pointer(self.t, n, px, py)
        self.t += n
        return self

    def shoot(self, n=6):
        return self.hold("B", n)

    # ---- player actions (the intended control mapping is in FORMAT.md; all of them are sequences of the primitives)
    def jump(self, hold=14, settle=40):
        self.hold("A", hold)
        return self.wait(settle)

    def spin(self, n=6, settle=78):
        self.tl.set_spin(self.t, n)
        self.t += n
        return self.wait(settle)

    def crouch(self, n):
        return self.hold("Z", n)

    def long_jump(self, d="F", runin=45, air=30, settle=50, mag=1.0):
        """Run d for `runin` turns, then Z (one turn ahead) and A with the stick still held, `air` turns of stick, settle."""
        a = math.radians(dir_angle(d))
        sx, sy = mag * math.sin(a), mag * math.cos(a)
        self.tl.set_stick(self.t, runin + 1 + air, sx, sy)
        self.tl.set_buttons(self.t + runin, 12, BUTTONS["Z"])
        self.tl.set_buttons(self.t + runin + 1, 14, BUTTONS["A"])
        self.t += runin + 1 + air
        return self.wait(settle)

    def backflip(self, settle=50, crouch=8, hold=14):
        """Z (crouch) while standing, A `crouch` turns later; Z is let go in the air."""
        self.tl.set_buttons(self.t, crouch + hold // 2, BUTTONS["Z"])
        self.tl.set_buttons(self.t + crouch, hold, BUTTONS["A"])
        self.t += crouch + hold
        return self.wait(settle)

    def ground_pound(self, settle=45, hold=14, rise=24, zturns=8):
        self.tl.set_buttons(self.t, hold, BUTTONS["A"])
        self.tl.set_buttons(self.t + rise, zturns, BUTTONS["Z"])
        self.t += rise + zturns
        return self.wait(settle)

    # ---- camera (D-pad LEFT/RIGHT = a 45 degree step, C = behind Mario, UP/DOWN = first person in / out)
    def cam_left(self, settle=48):
        self.hold("LEFT", 3)
        return self.wait(settle)

    def cam_right(self, settle=48):
        self.hold("RIGHT", 3)
        return self.wait(settle)

    def cam_orbit(self, steps=8, side="right", gap=50):
        for _ in range(steps):
            (self.cam_right if side == "right" else self.cam_left)(gap - 3)
        return self

    def cam_reset(self, settle=48):
        self.hold("C", 3)
        return self.wait(settle)

    def fp(self, look_turns=360, mag=1.0, enter_wait=40, exit_wait=40):
        """First-person view: UP, a look sweep with the stick (right, left, up, down, then a circle), DOWN."""
        self.hold("UP", 3).wait(enter_wait)
        t_end = self.t + look_turns
        legs = [("R", 0.2), ("L", 0.3), ("R", 0.1), ("F", 0.1), ("B", 0.2), ("F", 0.1)]     # shares sum to 1: right, left, back to the middle, up, down, up
        done = 0
        for i, (d, share) in enumerate(legs):
            n = look_turns - done if i == len(legs) - 1 else int(look_turns * share)
            assert n > 0, "fp: look_turns %d is too short" % look_turns
            self.run(d, n, mag)
            done += n
        assert self.t == t_end
        self.hold("DOWN", 3)
        return self.wait(exit_wait)

    def pause(self, turns=100, after=60):
        self.hold("PLUS", 4).wait(turns).hold("PLUS", 4)
        return self.wait(after)

    # ---- movement shapes
    def circle(self, n, per_rev=240, mag=1.0, cw=True, start="F", step=10):
        """The stick direction turns once round every per_rev turns (in `step`-turn pieces): Mario runs a ring."""
        a0 = dir_angle(start)
        sign = 1 if cw else -1
        t = 0
        while t < n:
            m = min(step, n - t)
            ang = a0 + sign * 360.0 * (t + m / 2.0) / per_rev
            self.run(ang, m, mag)
            t += m
        return self

    def wiggle(self, da, db, period, n, mag=1.0):
        t = 0
        k = 0
        while t < n:
            m = min(period, n - t)
            self.run(da if k % 2 == 0 else db, m, mag)
            t += m
            k += 1
        return self

    def box(self, leg=60, mag=1.0, jumps=False, pause_between=10):
        """Forward, right, back, left: LEG turns each (net zero in the camera's frame), with a rest between the legs."""
        for i, d in enumerate(["F", "R", "B", "L"]):
            if jumps and i in (0, 2):
                with self.par() as p:
                    p.run(d, leg, mag)
                    p.at(max(leg // 2 - 8, 1)).hold("A", 14)
            else:
                self.run(d, leg, mag)
            self.wait(pause_between)
        return self

    def par(self):
        return Par(self)


class Par:
    """with r.par() as p: p.run(...); p.at(30).jump(); ...   Everything starts at the cursor (plus its own offset); the
    cursor then moves to the end of the longest one."""

    def __init__(self, parent):
        self.parent = parent
        self.base = parent.t
        self.subs = []
        self.main = Seg(parent.tl, parent.t)
        self.subs.append(self.main)

    def at(self, k):
        s = Seg(self.parent.tl, self.base + k)
        self.subs.append(s)
        return s

    def __getattr__(self, name):         # p.run(...) is p.at(0).run(...), all on one sequential sub
        return getattr(self.main, name)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.parent.t = max(s.t for s in self.subs)
        self.parent.tl.grow(self.parent.t)
        return False


class Route(Seg):
    def __init__(self, name="route"):
        super().__init__(Timeline(), 0)
        self.name = name

    @property
    def turns(self):
        return max(self.t, len(self.tl))

    # ---- compile to entries
    def entries(self, flat=False):
        """[(t0_turn, t1_turn, items)] with items a list of strings.  Layered (default): the stick, each button set, the spin
        and the pointer are separate runs that overlap (buttons OR, the stick of the later entry wins: FORMAT.md).  flat:
        one entry per change of the whole state (more entries, nothing overlaps)."""
        tl, n = self.tl, self.turns
        tl.grow(n)
        out = []

        def runs(key):
            res, t = [], 0
            while t < n:
                v = key(t)
                u = t + 1
                while u < n and key(u) == v:
                    u += 1
                res.append((t, u, v))
                t = u
            return res

        def stick_items(v):
            return ["SX=" + fmt_f(v[0]), "SY=" + fmt_f(v[1])]

        def point_items(v):
            return ["PX=" + fmt_f(v[0]), "PY=" + fmt_f(v[1])]

        def btn_items(m):
            return [b for b in BUTTON_ORDER if m & BUTTONS[b]]

        if flat:
            for a, b, v in runs(lambda t: (tl.buttons[t], tl.stick[t], tl.spin[t], tl.pointer[t])):
                m, s, sp, p = v
                items = btn_items(m) + (["SPIN"] if sp else []) + (stick_items(s) if s else []) + (point_items(p) if p else [])
                if items:
                    out.append((a, b, items))
            return out
        for a, b, v in runs(lambda t: tl.stick[t]):
            if v:
                out.append((a, b, stick_items(v)))
        for a, b, v in runs(lambda t: tl.buttons[t]):
            if v:
                out.append((a, b, btn_items(v)))
        for a, b, v in runs(lambda t: tl.spin[t]):
            if v:
                out.append((a, b, ["SPIN"]))
        for a, b, v in runs(lambda t: tl.pointer[t]):
            if v:
                out.append((a, b, point_items(v)))
        out.sort(key=lambda e: (e[0], e[1]))
        return out

    def lines(self, flat=False):
        res = []
        for a, b, items in self.entries(flat):
            res.append("%s-%s:%s" % (fmt_ms(ms_of(a)), fmt_ms(ms_of(b)), "|".join(items)))
        return res

    def text(self, header=(), flat=False):
        body = self.lines(flat)
        head = ["# " + h for h in header]
        head.append("# mkroute.py: %s, %d turns (%.2f s), %d entries; clock = game turns, 60 per second" %
                    (self.name, self.turns, self.turns / TPS, len(body)))
        return "\n".join(head + body) + "\n"


# ----------------------------------------------------------------------------------------------------------------------
# Strict re-implementation of the port's loader (vita/vgx-game/entry.cpp LoadInputScript) and parser/applier
# (platform/include/port/input_script.h).  tools/test_mkroute.py also checks it against the REAL header compiled on the host.
# ----------------------------------------------------------------------------------------------------------------------
class ScriptError(Exception):
    pass


def load_joined(text, max_chars=STOCK_CHARS):
    """LoadInputScript: leading blanks skipped; blank lines and '#' lines dropped; trailing \\n \\r space cut; lines joined by ','.
    A line longer than 511 characters is cut by fgets and the rest becomes a line of its own (joined with a comma)."""
    lines = []
    for raw in text.split("\n"):
        raw = raw + "\n"
        while len(raw) > 511:
            lines.append(raw[:511])
            raw = raw[511:]
        lines.append(raw)
    joined, used, cut = "", 0, False
    for line in lines:
        s = line.lstrip(" \t")
        if not s or s[0] in "#\n\r":
            continue
        s = s.rstrip("\n\r ")
        piece = ("," if used else "") + s
        if used + len(piece) >= max_chars:
            # snprintf(joined + used, sizeof - used, ...) does not fit: it still WRITES the first (sizeof - used - 1) characters
            # (NUL-terminated), the loader stops, and the parser then reads that cut-off piece as a last entry
            joined += piece[:max_chars - used - 1]
            cut = True
            break
        joined += piece
        used += len(piece)
    return joined, cut


def _atof(s):
    m = re.match(r"\s*[+-]?(\d+\.?\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?)", s)
    return float(m.group(0)) if m else 0.0


def parse_joined(joined, max_entries=STOCK_ENTRIES, strict=True):
    """portParseInputScript.  Returns events as dicts; with strict=True anything the C parser would only print to stderr
    (and, for an unknown item, skip) raises ScriptError."""
    events = []
    for tok in joined.split(","):
        if not tok:
            continue
        if len(events) >= max_entries:
            if strict:
                raise ScriptError("more than %d entries: the rest is dropped" % max_entries)
            break
        if ":" not in tok:
            if strict:
                raise ScriptError("bad entry %r (no ':')" % tok)
            continue
        head, rest = tok.split(":", 1)
        e = {"t0": int(_atof(head) * 1000.0), "buttons": 0, "spin": 0, "stick": None, "pointer": None, "tilt": None}
        if "-" in head:
            e["t1"] = int(_atof(head.split("-", 1)[1]) * 1000.0)
        else:
            e["t1"] = e["t0"] + 150
        sx = sy = px = py = pitch = roll = 0.0
        hs = hp = ht = False
        for item in rest.split("|"):
            if not item:
                continue
            if item.startswith("SX="):
                hs, sx = True, _atof(item[3:])
            elif item.startswith("SY="):
                hs, sy = True, _atof(item[3:])
            elif item.startswith("PX="):
                hp, px = True, _atof(item[3:])
            elif item.startswith("PY="):
                hp, py = True, _atof(item[3:])
            elif item.startswith("PITCH="):
                ht, pitch = True, _atof(item[6:])
            elif item.startswith("ROLL="):
                ht, roll = True, _atof(item[5:])
            elif item == "SPIN":
                e["spin"] = 1
            elif item in BUTTONS:
                e["buttons"] |= BUTTONS[item]
            elif strict:
                raise ScriptError("unknown item %r in %r" % (item, tok))
        if hs:
            e["stick"] = (sx, sy)
        if hp:
            e["pointer"] = (px, py)
        if ht:
            e["tilt"] = (pitch, roll)
        if strict:
            if e["t1"] <= e["t0"]:
                raise ScriptError("empty or reversed range in %r" % tok)
            if e["stick"] and (abs(e["stick"][0]) > 1.0 or abs(e["stick"][1]) > 1.0):
                raise ScriptError("stick beyond 1.0 in %r" % tok)
            if not (e["buttons"] or e["spin"] or e["stick"] or e["pointer"] or e["tilt"]):
                raise ScriptError("entry with no effect %r" % tok)
        events.append(e)
    return events


def apply_at(events, ms):
    """portApplyInputScript at `ms`: the state the pad gets (buttons OR-ed, later stick/pointer entries win, SPIN)."""
    st = {"buttons": 0, "stick": None, "spin": False, "pointer": None}
    for e in events:
        if ms < e["t0"] or ms >= e["t1"]:
            continue
        st["buttons"] |= e["buttons"]
        if e["stick"]:
            st["stick"] = e["stick"]
        if e["pointer"]:
            st["pointer"] = e["pointer"]
        if e["spin"]:
            st["spin"] = True
    return st


def check_text(text, max_entries=STOCK_ENTRIES, max_chars=STOCK_CHARS):
    """Strict validation of a whole input.txt; returns (events, info dict)."""
    for i, raw in enumerate(text.split("\n")):
        if len(raw) > LINE_MAX:
            raise ScriptError("line %d is %d characters: the loader's 512-byte fgets would cut it" % (i + 1, len(raw)))
    joined, cut = load_joined(text, max_chars)
    if cut:
        raise ScriptError("the file is longer than the loader's %d-character buffer: the tail is dropped" % max_chars)
    events = parse_joined(joined, max_entries, strict=True)
    if not events:
        raise ScriptError("no entries")
    end_ms = max(e["t1"] for e in events)
    return events, {"entries": len(events), "chars": len(joined), "end_ms": end_ms, "turns": -(-end_ms * TPS // 1000)}


def replay(events, turns):
    """The state the port applies on each of the first `turns` turns (ms = turn*1000//60)."""
    return [apply_at(events, ms_of(t)) for t in range(turns)]


def intended(route, turns=None):
    tl = route.tl
    n = turns if turns is not None else route.turns
    res = []
    for t in range(n):
        if t < len(tl):
            res.append({"buttons": tl.buttons[t], "stick": tl.stick[t], "spin": tl.spin[t], "pointer": tl.pointer[t]})
        else:
            res.append({"buttons": 0, "stick": None, "spin": False, "pointer": None})
    return res


def verify_route(route, flat=False, max_entries=STOCK_ENTRIES, max_chars=STOCK_CHARS):
    """Emit, re-parse and replay: every turn's state must be the intended one (turn-accurate proof).  Returns the info dict."""
    text = route.text(flat=flat)
    events, info = check_text(text, max_entries, max_chars)
    got = replay(events, route.turns + 2)
    want = intended(route, route.turns + 2)
    for t, (g, w) in enumerate(zip(got, want)):
        gs = g["stick"]
        ws = w["stick"]
        same_stick = (gs is None and ws is None) or (gs is not None and ws is not None and
                                                       abs(gs[0] - ws[0]) < 1e-9 and abs(gs[1] - ws[1]) < 1e-9)
        same_ptr = (g["pointer"] is None and w["pointer"] is None) or (g["pointer"] is not None and w["pointer"] is not None and
                                                                      abs(g["pointer"][0] - w["pointer"][0]) < 1e-9 and
                                                                      abs(g["pointer"][1] - w["pointer"][1]) < 1e-9)
        if g["buttons"] != w["buttons"] or g["spin"] != w["spin"] or not same_stick or not same_ptr:
            raise ScriptError("turn %d: the port would apply %r, intended %r" % (t, g, w))
    return info


def warnings(route):
    """Things the game itself will not do the way a script hopes (see FORMAT.md 'Game rules that bite')."""
    tl, n = route.tl, route.turns
    w = []
    last_ab = -1000
    last_spin = -1000
    prev_a = False
    prev_spin = False
    for t in range(n):
        a = bool(tl.buttons[t] & (BUTTONS["A"] | BUTTONS["B"]))
        if a and not prev_a:
            last_ab = t
        if tl.spin[t] and not prev_spin:
            if t - last_ab < 10:
                w.append("turn %d: a SPIN %d turns after an A/B press: the game ignores a swing within 10 turns of A/B (the port holds it back)" % (t, t - last_ab))
            if t - last_spin < 84:
                w.append("turn %d: a SPIN %d turns after the last one: the port lets one pulse start only every ~84 turns" % (t, t - last_spin))
            last_spin = t
        prev_a, prev_spin = a, bool(tl.spin[t])
        if tl.buttons[t] & (BUTTONS["HOME"] | BUTTONS["MINUS"]):
            w.append("turn %d: HOME/MINUS are not game buttons here (MINUS opens the pause menu, HOME is unused)" % t)
    return w


# ----------------------------------------------------------------------------------------------------------------------
# Segment-file reader
# ----------------------------------------------------------------------------------------------------------------------
def _num(s):
    return float(s) if ("." in s) else int(s)


def run_commands(seg, lines, i=0, until_end=False):
    """Execute spec lines on `seg`; returns the index after the matching 'end' (or the end)."""
    while i < len(lines):
        raw = lines[i]
        line = raw.split("#", 1)[0].strip()
        i += 1
        if not line:
            continue
        w = shlex.split(line)
        c = w[0].lower()
        a = w[1:]
        if c == "end":
            if until_end:
                return i
            raise ScriptError("'end' without a block")
        if c == "par":
            with seg.par() as p:
                i = _run_par(p, lines, i)
            continue
        if c == "repeat":
            k = int(a[0])
            start = i
            for _ in range(k):
                j = run_commands(seg, lines, start, until_end=True)
            i = j if k else _skip_block(lines, start)
            continue
        _command(seg, c, a)
    if until_end:
        raise ScriptError("missing 'end'")
    return i


def _skip_block(lines, i):
    depth = 1
    while i < len(lines):
        w = lines[i].split("#", 1)[0].split()
        if w and w[0].lower() in ("par", "repeat"):
            depth += 1
        if w and w[0].lower() == "end":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ScriptError("missing 'end'")


def _run_par(p, lines, i):
    cur = p.main
    while i < len(lines):
        line = lines[i].split("#", 1)[0].strip()
        i += 1
        if not line:
            continue
        w = shlex.split(line)
        if w[0].lower() == "end":
            return i
        if w[0].lower() == "at":
            tgt = p.at(int(w[1]))
            _command(tgt, w[2].lower(), w[3:])
        else:
            _command(cur, w[0].lower(), w[1:])
    raise ScriptError("missing 'end'")


def _command(seg, c, a):
    n = lambda k, d: (_num(a[k]) if len(a) > k else d)
    if c == "wait":
        seg.wait(int(a[0]))
    elif c == "run":
        seg.run(a[0], int(a[1]), n(2, 1.0))
    elif c == "walk":
        seg.walk(a[0], int(a[1]), n(2, 0.5))
    elif c == "stick":
        seg.stick(float(a[0]), float(a[1]), int(a[2]))
    elif c == "hold":
        seg.hold(a[0], int(a[1]))
    elif c == "press":
        seg.press(a[0], int(n(1, 6)))
    elif c == "jump":
        seg.jump(int(n(0, 14)), int(n(1, 40)))
    elif c == "spin":
        seg.spin(int(n(0, 6)), int(n(1, 78)))
    elif c == "longjump":
        seg.long_jump(a[0] if a else "F", int(n(1, 45)), int(n(2, 30)), int(n(3, 50)))
    elif c == "backflip":
        seg.backflip(int(n(0, 50)))
    elif c == "groundpound":
        seg.ground_pound(int(n(0, 45)))
    elif c == "crouch":
        seg.crouch(int(a[0]))
    elif c == "camleft":
        seg.cam_left(int(n(0, 48)))
    elif c == "camright":
        seg.cam_right(int(n(0, 48)))
    elif c == "camorbit":
        seg.cam_orbit(int(n(0, 8)), a[1] if len(a) > 1 else "right", int(n(2, 50)))
    elif c == "camreset":
        seg.cam_reset(int(n(0, 48)))
    elif c == "fp":
        seg.fp(int(a[0]), n(1, 1.0))
    elif c == "pause":
        seg.pause(int(n(0, 100)))
    elif c == "circle":
        seg.circle(int(a[0]), int(n(1, 240)), n(2, 1.0), (a[3].lower() != "ccw") if len(a) > 3 else True)
    elif c == "wiggle":
        seg.wiggle(a[0], a[1], int(a[2]), int(a[3]), n(4, 1.0))
    elif c == "point":
        seg.pointer(float(a[0]), float(a[1]), int(a[2]))
    elif c == "shoot":
        seg.shoot()
    elif c == "box":
        seg.box(int(n(0, 60)), n(1, 1.0), bool(int(n(2, 0))))
    else:
        raise ScriptError("unknown command %r" % c)


def build_from_spec(text, name="spec"):
    r = Route(name)
    run_commands(r, text.split("\n"))
    return r


# ----------------------------------------------------------------------------------------------------------------------
def summary(events, info):
    keys = {}
    for e in events:
        for b in BUTTON_ORDER:
            if e["buttons"] & BUTTONS[b]:
                keys[b] = keys.get(b, 0) + 1
    spins = sum(1 for e in events if e["spin"])
    sticks = sum(1 for e in events if e["stick"])
    pts = sum(1 for e in events if e["pointer"])
    return ("%d entries, %d chars, ends at %.3f s (%d turns); stick entries %d, spin entries %d, pointer entries %d; button entries: %s" %
            (info["entries"], info["chars"], info["end_ms"] / 1000.0, info["turns"], sticks, spins, pts,
             " ".join("%s=%d" % kv for kv in sorted(keys.items())) or "none"))


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd = argv[1]
    stock = "--stock" in argv
    lim = (STOCK_ENTRIES, STOCK_CHARS)
    if cmd == "build":
        args = [a for a in argv[2:] if a != "--stock"]
        out = None
        if "-o" in args:
            k = args.index("-o")
            out = args[k + 1]
            del args[k:k + 2]
        text = sys.stdin.read() if args[0] == "-" else open(args[0]).read()
        r = build_from_spec(text, os.path.basename(args[0]))
        info = verify_route(r, False, *lim)
        data = r.text()
        for wmsg in warnings(r):
            print("warning:", wmsg, file=sys.stderr)
        if out:
            open(out, "w").write(data)
            print("%s: %d entries, %d chars, %d turns (%.1f s)" % (out, info["entries"], info["chars"], r.turns, r.turns / TPS))
        else:
            sys.stdout.write(data)
        return 0
    if cmd == "check":
        text = open(argv[2]).read()
        try:
            events, info = check_text(text, *lim)
        except ScriptError as e:
            print("INVALID:", e)
            return 1
        print("OK:", summary(events, info))
        return 0
    if cmd == "selftest":
        import unittest
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import test_mkroute
        suite = unittest.defaultTestLoader.loadTestsFromModule(test_mkroute)
        res = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if res.wasSuccessful() else 1
    print("unknown command", cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""plan.py - the planner and log analyser of "seedwalk", the emulator-driven harvest walk of every galaxy (plan-perf31 A.3 / A1).

Pure Python 3.8+, no network, no emulator. Run with python3 -I. Everything it writes goes under the --out folder it is given.

  plan.py make   --out OUT [options]    split the stage table into legs; write OUT/plan.json and OUT/legs/<id>/{spec.json,env.txt,input.txt}
  plan.py list   OUT                    one line per leg (id, kind, mode, stops, estimated wall time)
  plan.py estimate OUT                  wall-time estimate per leg and in total (low / typical / high)
  plan.py check-env FILE [FILE...]      the env allow-list: exit 0 when every file passes, 3 with the reasons otherwise
  plan.py leginfo OUT ID                shell-safe KEY=VALUE lines for leg.sh
  plan.py leg-status OUT ID             read the leg's copied log, write legs/<id>/status.json (and DONE when complete); exit 0 complete, 4 incomplete
  plan.py resume OUT                    one follow-up leg (<id>-rN) for every leg that stopped short: only the stops it did not finish
  plan.py selftest                      the unit tests and the dry run against the fake log fixture (fixtures/)

THE STAGE TABLE is tools/pack24/tour_legs.txt (one "Galaxy:scenario" per stop, comma separated, several lines). A leg is 1..24 stops of the
same kind (PETARI_FLOW takes at most 24 entries and a 2 KiB line). Kinds:
  flow     PETARI_BOOT=flow + PETARI_FLOW=<stops>: the star select, as a player enters (hidden stars as the tour183 fix does)
  stage    PETARI_BOOT=1 + PETARI_STAGE=<stops>: the galaxies that have no star select of their own (dome, observatory, library, castle)
  menus    title, file select, hub, galaxy map (PETARI_FLOW_DOME); makes the save template
  mpmenus  SMG_MP_DEMO=menus: the entry screen, the Multiplayer menu's screens and (harvprep31, with GXV_SEEDWALK=1) every native Settings page, each once,
           by the game's own tour (decomp/include/Game/MP/MpHarvestTour.hpp): nothing chosen, changed, written or sent
  mpdemo   SMG_MP_DEMO=1 in three stages
A pass is (picture mode, zoom): by default p60:1.5 (Performance, the handheld zoom) and q30:1.0 (Quality, zoom 1.0). Every stop is walked once per pass.

THE ENV ALLOW-LIST (check_env) is default-deny: PETARI_*, SMG_MP_DEMO*, logging keys (*_LOG), the cache tag, the harvest switch, the list paths, the
non-text emulator keys, GXV_PICTURE_MODE (30 | 60), GXV_STAGE_ENUM (0-2). Any shader-text switch (GXV_VSOUT, GXV_TEXVEC2, GXV_TEVIN_CANON, ...), even
set to its retail value, and any forced pack (GXV_SHADERPACK, GXV_VITA_SEED, GXV_USERPACK, GXV_NO_SHADERLIST) is refused with the reason: a walk
in another shader profile than the release's makes lists and files that pass every format check and are dead weight (plan-perf31 R3).
"""
import hashlib
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_TOOLS = HERE.parent
DEFAULT_TABLE = REPO_TOOLS / "pack24" / "tour_legs.txt"
REFILL = REPO_TOOLS / "pack24" / "refill_pack.py"
FORMAT = 1
HARVEST_KEY = "GXV_SEEDWALK"      # the game-side harvest switch (seedrun31): see README.md "Interface expected from other agents"

# ---- the env allow-list -------------------------------------------------------------------------------------------------------------------
# Shader-text switches: the generator inputs that change a key or a text. Union of refill_pack.py's own list (read from its source when the tree has
# it) and this built-in copy, so a switch added to either side is refused.
BUILTIN_TEXT_SWITCHES = ("GXV_TEVIN_CANON", "GXV_FS_FINCLAMP", "GXV_VSOUT", "GXV_TEXVEC2", "GXV_FOG_BIND", "GXV_FOGVTX", "GXV_FS_HALF",
                         "GXV_FS_HALF_MINOR", "GXV_FS_TRIM_PARTS", "GXV_DISCARD_BLEND", "GXV_IND_VS", "GXV_TEVWRAP", "GXV_FS_PROJ",
                         "GXV_FS_ADEAD", "GXV_FS_TEXREUSE")

def text_switches():
    s = list(BUILTIN_TEXT_SWITCHES)
    try:
        src = REFILL.read_text()
        m = re.search(r"TEXT_SWITCHES\s*=\s*\(([^)]*)\)", src)
        if m:
            s += re.findall(r'"(GXV_[A-Z0-9_]+)"', m.group(1))
    except OSError:
        pass
    return tuple(dict.fromkeys(s))

FORCED_PACK = {"GXV_SHADERPACK": "forces the package's RAM pack on or off", "GXV_VITA_SEED": "forces the package's loose seed on or off",
               "GXV_USERPACK": "forces the card's user pack", "GXV_NO_SHADERLIST": "drops the key list",
               "GXV_VITA_SEED_OFF": "forces the seed off", "GXV_SEED_OFF": "forces the seed off"}
# exact keys with the values they may take (None = any)
ALLOW_EXACT = {
    "GXV_PICTURE_MODE": ("30", "60"), "GXV_STAGE_ENUM": ("0", "1", "2"), "GXV_PACK_MISSLOG": ("1",), "GXV_FIRSTUSE_LOG": None,
    "GXV_SHADER_CACHE_TAG": "tag", "GXV_STAGEWARM_FILE": "path", "GXV_SHADERLIST": "path", "GXV_PLAYERFILE": "path",
    "GXV_STDERR_FILE": ("0", "1"), "GXV_VITA_SPLASH": ("0", "1"), "GXV_KEEP_RT": ("0", "1"), "GXV_VITA_MASK_GUARD": None,
    "SMG_MP_DEMO": ("1", "menus"), "SMG_MP_DEMO_SECS": None, HARVEST_KEY: None,
}
ALLOW_PREFIX = ("PETARI_", HARVEST_KEY + "_")
LOGKEY = re.compile(r"^GXV_[A-Z0-9_]*_LOG$")

def parse_env_lines(text):
    """-> ([(key, value, lineno)], [problems]); '#' comments and blank lines ignored"""
    out, bad = [], []
    for n, raw in enumerate(text.replace("\r", "").split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            bad.append(f"line {n}: not KEY=VALUE: {line[:60]!r}")
            continue
        k, v = line.split("=", 1)
        out.append((k.strip(), v.strip(), n))
    return out, bad

def check_env_text(text, switches=None):
    """-> list of problems (empty = allowed)"""
    sw = switches if switches is not None else text_switches()
    items, bad = parse_env_lines(text)
    seen = {}
    for k, v, n in items:
        if k in seen:
            bad.append(f"line {n}: {k} set twice (first on line {seen[k]})")
        seen[k] = n
        if k in sw:
            bad.append(f"line {n}: {k}={v}: a shader-text switch (it changes keys or texts; the walk must run in the retail profile; not even its retail value may be set)")
        elif k in FORCED_PACK:
            bad.append(f"line {n}: {k}={v}: a forced pack ({FORCED_PACK[k]}); the walk starts from the candidate seed as shipped")
        elif k in ALLOW_EXACT:
            rule = ALLOW_EXACT[k]
            if isinstance(rule, tuple) and v not in rule:
                bad.append(f"line {n}: {k}={v}: allowed values are {'/'.join(rule)}")
            elif rule == "tag" and not re.fullmatch(r"[A-Za-z0-9]{1,12}", v):
                bad.append(f"line {n}: {k}={v}: 1-12 letters or digits")
            elif rule == "path" and not (v.startswith("ux0:data/smg/") and ".." not in v):
                bad.append(f"line {n}: {k}={v}: a list path must be under ux0:data/smg/ (the walk's own card folder)")
        elif any(k.startswith(p) for p in ALLOW_PREFIX) or LOGKEY.match(k):
            if k in ("PETARI_SHADERPACK", ) or "VSOUT" in k:
                bad.append(f"line {n}: {k}: refused")
        else:
            bad.append(f"line {n}: {k}={v}: not on the allow-list (PETARI_*, SMG_MP_DEMO*, *_LOG, cache tag, list paths, GXV_PICTURE_MODE, {HARVEST_KEY}*)")
    return bad

# ---- the stage table ----------------------------------------------------------------------------------------------------------------------
STOP = re.compile(r"^[A-Za-z][A-Za-z0-9]*:[1-9][0-9]?$")
NO_STAR_SELECT = ("AstroDome", "AstroGalaxy", "LibraryRoom", "PeachCastleFinalGalaxy", "PeachCastleGardenGalaxy")   # make_jobs.py S0_G: PETARI_STAGE, not the flow

def read_table(path=None):
    p = Path(path) if path else DEFAULT_TABLE
    stops = []
    for line in p.read_text().split():
        for s in line.split(","):
            s = s.strip()
            if s:
                stops.append(s)
    bad = [s for s in stops if not STOP.match(s)]
    if bad:
        raise SystemExit(f"plan.py: {p}: not Galaxy:scenario: {bad[:5]}")
    dup = sorted({s for s in stops if stops.count(s) > 1})
    if dup:
        raise SystemExit(f"plan.py: {p}: duplicate entries {dup[:5]}")
    return stops

def split(xs, per):
    """balanced chunks of at most per items (no tiny tail)"""
    if not xs:
        return []
    k = -(-len(xs) // per)
    n, r = divmod(len(xs), k)
    out, i = [], 0
    for j in range(k):
        m = n + (1 if j < r else 0)
        out.append(xs[i:i + m]); i += m
    return out

# ---- the scripted route (mkroute.py of tools/pack24) ---------------------------------------------------------------------------------------
PU_LIST = "4,0,3,0,5,0,1,0,6,0,2,0,7,0"     # bee, cure, ice, cure, spring, cure, rainbow, cure, boo, cure, fire, cure, flying, cure (make_jobs.py)
PU_AT, PU_EVERY = 240, 300
MAPS = [{}, {"F": "R", "B": "L", "L": "F", "R": "B"}]    # route variant 0 = the pack24 route; 1 = turned a quarter (the hold-out walk, A.5 item 2)

def build_route(total_turns, variant=0):
    """the pack24 action route (run, jump, spin, run+jump, long jump, backflip, pointer + star-bit shots, one pause, crouch, ground pound, camera steps),
    then a player-form cycle every 300 turns until total_turns. t = 0 is the first turn of play (PETARI_SCRIPT_ANCHOR=play)."""
    sys.path.insert(0, str(REPO_TOOLS / "pack24"))
    from mkroute import Route, verify_route, STOCK_ENTRIES, STOCK_CHARS  # noqa: E402
    m = MAPS[variant % len(MAPS)]
    d = lambda x: m.get(x, x)
    L = lambda n: max(4, int(round(n * 0.5)))
    r = Route("seedwalk")
    r.wait(6)
    r.run(d("F"), L(40)); r.wait(20)
    r.jump(14, 40)
    r.spin(6, 40)
    with r.par() as p:
        p.run(d("B"), L(40)); p.at(L(40) // 2 + 2).hold("A", 14)
    r.wait(40)
    r.long_jump(d("F"), L(30), 25, 50)
    r.run(d("B"), L(70)); r.wait(20)
    r.backflip(50)
    r.run(d("F"), L(25)); r.wait(30)
    for x, y in ((-0.5, 0.2), (0.5, -0.2)):
        with r.par() as p:
            p.pointer(x, y, 90); p.at(30).shoot(); p.at(60).shoot()
    r.wait(30)
    r.pause(100, 60)
    r.crouch(40); r.wait(20)
    r.jump(14, 10); r.ground_pound(60)
    r.cam_right(); r.cam_left(); r.cam_reset()
    r.wait(max(1500 - r.t, 60))
    while r.t + PU_EVERY <= total_turns:           # the form cycle of route-pu-p24, every PETARI_POWERUP_EVERY turns
        t0 = r.t
        r.run(d("F"), L(40)); r.jump(40, 20); r.spin(6, 30)
        with r.par() as p:
            p.pointer(0.4, 0.1, 60); p.at(20).shoot(); p.at(40).shoot()
        r.run(d("B"), L(40)); r.jump(14, 20); r.ground_pound(45)
        r.wait(max(PU_EVERY - (r.t - t0), 1))
    r.wait(max(total_turns - r.t, 1))
    verify_route(r, False, STOCK_ENTRIES, STOCK_CHARS); verify_route(r, True, STOCK_ENTRIES, STOCK_CHARS)
    head = [f"seedwalk route (variant {variant}): the pack24 first-use block, pointer + shots, one pause, crouch, ground pound, camera, then a form cycle every {PU_EVERY} turns",
            f"{r.turns} turns ({r.turns / 60:.1f} s); t = 0 is the first turn of play in the scene (PETARI_SCRIPT_ANCHOR=play); made by tools/seedwalk/plan.py with tools/pack24/mkroute.py"]
    return r.text(header=head)

# ---- the plan -----------------------------------------------------------------------------------------------------------------------------
def parse_passes(s):
    out = []
    for item in s.split(","):
        mode, _, zoom = item.partition(":")
        if mode not in ("p60", "q30"):
            raise SystemExit(f"plan.py: pass {item!r}: mode must be p60 (Performance) or q30 (Quality)")
        out.append({"id": mode, "picture_mode": 60 if mode == "p60" else 30, "zoom": zoom or ("1.5" if mode == "p60" else "1.0")})
    return out

def flow_line(stops):
    line = ",".join(stops)
    if len(stops) > 24 or len(line) > 1900:
        raise SystemExit(f"plan.py: a leg of {len(stops)} stops / {len(line)} characters is more than PETARI_FLOW takes (24 entries, 2 KiB)")
    return line

def leg_env(kind, ps, stops, a):
    """the env file of a leg, as an ordered list of (key, value)"""
    e = [("PETARI_GAME_CLOCK", "1")]
    if kind in ("flow", "stage", "mpdemo"):
        e += [("PETARI_SCRIPT_ANCHOR", "play"), ("PETARI_SCRIPT_LOG", "1")]
    else:
        e += [("PETARI_SCRIPT_ANCHOR_DRAWS", "18")]
    e += [("GXV_PICTURE_MODE", str(ps["picture_mode"])), ("PETARI_ZOOM_FORCE", ps["zoom"]),
          ("GXV_SHADER_CACHE_TAG", "sw31"), ("GXV_PACK_MISSLOG", "1"), ("GXV_FIRSTUSE_LOG", "1"), ("PETARI_FLOW_LOG", "1")]
    if a["harvest"]:
        e.append((HARVEST_KEY, "1"))
    if a["enum"]:
        e.append(("GXV_STAGE_ENUM", str(a["enum"])))
    if kind in ("flow", "stage"):
        e += [("PETARI_BOOT", "flow" if kind == "flow" else "1")]
        if kind == "flow":
            e += [("PETARI_FLOW", flow_line(stops)), ("PETARI_FLOW_HOLD", str(a["hold"]))]
        else:
            e += [("PETARI_STAGE", flow_line(stops)), ("PETARI_STAGE_HOLD", str(a["hold"]))]
        e += [("PETARI_SURVEY_HOP", f"{a['hop']:g}"), ("PETARI_SURVEY_MAX", str(a["hop_max"])), ("PETARI_SURVEY_GPOS", "1"),
              ("PETARI_POWERUP", PU_LIST), ("PETARI_POWERUP_AT", str(PU_AT)), ("PETARI_POWERUP_EVERY", str(PU_EVERY))]
    elif kind == "menus":
        e += [("PETARI_FLOW", flow_line(stops)), ("PETARI_FLOW_HOLD", "600"), ("PETARI_FLOW_DOME", "1")]
    elif kind == "mpmenus":
        e += [("SMG_MP_DEMO", "menus")]
    elif kind == "mpdemo":
        e += [("PETARI_BOOT", "flow"), ("PETARI_FLOW", flow_line(stops)), ("PETARI_FLOW_HOLD", "2400"), ("SMG_MP_DEMO", "1")]
    return e

# wall-time model (seconds): see estimate_leg. speed = game s per wall s of the emulator (README.txt of the emulator setup, $V3K: 0.35 on a quiet machine)
# (harvprep31, measured: the first real leg, integ31 34ee2865 in Vita3K with the test19 seed, 17 Nov: play 0.21 game s per wall s, a stop's load and opening camera
# 171 s, the boot to the save's first request ~70 s; the earlier typical guess was 0.35 / 150 / 100. "high" is a busy machine.)
MODEL = {"low": {"speed": 0.35, "load": 120, "boot": 60, "menu_boot": 480},
         "typ": {"speed": 0.21, "load": 175, "boot": 80, "menu_boot": 600},
         "high": {"speed": 0.12, "load": 300, "boot": 160, "menu_boot": 900}}

def estimate_leg(spec, which="typ"):
    m = MODEL[which]
    k = spec["kind"]
    n = len(spec["stops"])
    if k in ("flow", "stage"):
        return m["boot"] + n * (m["load"] + spec["hold"] / 60 / m["speed"]) + 30
    if k == "menus":
        return m["menu_boot"] + n * (m["load"] + 600 / 60 / m["speed"]) + m["load"] + 120
    if k == "mpmenus":
        return m["menu_boot"] + 300 / m["speed"]
    if k == "mpdemo":
        return m["boot"] + n * (m["load"] + 2400 / 60 / m["speed"]) + 30
    return 0

def make_plan(a):
    table = read_table(a["table"])
    passes = parse_passes(a["passes"])
    flow_stops = [s for s in table if s.split(":")[0] not in NO_STAR_SELECT]
    stage_stops = [s for s in table if s.split(":")[0] in NO_STAR_SELECT]
    # a leg is a chunk of one kind; hold >= what the survey hops need (hop_max hops of hop seconds, 120 turns settle)
    a["hold"] = max(a["hold"], int(120 + a["hop_max"] * a["hop"] * 60))
    legs = []
    v = a["variant"]
    def rot(xs):
        return xs[v % len(xs):] + xs[:v % len(xs)] if xs and v else list(xs)
    first = True
    for ps in passes:
        if first or a["menus_both"]:
            if not a["no_menus"]:
                legs.append({"kind": "menus", "ps": ps, "stops": ["EggStarGalaxy:1", "HoneyBeeKingdomGalaxy:1"]})
                legs.append({"kind": "mpmenus", "ps": ps, "stops": []})
                legs.append({"kind": "mpdemo", "ps": ps, "stops": ["EggStarGalaxy:1", "HoneyBeeKingdomGalaxy:1", "StarDustGalaxy:1"]})
        first = False
    for ps in passes:
        for kind, stops in (("flow", rot(flow_stops)), ("stage", rot(stage_stops))):
            for chunk in split(stops, a["per_leg"]):
                legs.append({"kind": kind, "ps": ps, "stops": chunk})
    count = {}
    out = []
    for lg in legs:
        key = (lg["ps"]["id"], lg["kind"])
        count[key] = count.get(key, 0) + 1
        lid = f"{lg['ps']['id']}-{lg['kind']}-{count[key]:02d}"
        env = leg_env(lg["kind"], lg["ps"], lg["stops"], a)
        spec = {"id": lid, "kind": lg["kind"], "pass": lg["ps"]["id"], "picture_mode": lg["ps"]["picture_mode"], "zoom": lg["ps"]["zoom"],
                "stops": lg["stops"], "hold": a["hold"] if lg["kind"] in ("flow", "stage") else 0, "boot": lg["kind"] in ("flow", "stage", "mpdemo"),
                "route_variant": v, "parent": None, "env": env}
        if lg["kind"] in ("flow", "stage", "mpdemo"):
            spec["input"] = "input.txt"
        elif lg["kind"] == "menus":
            spec["input"] = "menu-fast"          # leg.sh uses $V3K/inputs/menu-fast.txt
        else:
            spec["input"] = None
        stop = stop_rule(spec)
        spec.update(stop)
        spec["est_wall_s"] = round(estimate_leg(spec))
        out.append(spec)
    return table, out

def stop_rule(spec):
    """how leg.sh ends the emulator: STOPAT (grep -E on the game log) + STOPAFTER, and a wall cap"""
    k, n = spec["kind"], len(spec["stops"])
    if k in ("flow", "mpdemo", "menus"):
        stopat, after = r"flow: done: all [0-9]+ entries", "3"
    elif k == "stage":
        last = spec["stops"][-1].split(":")
        stopat, after = r"tour: jumping to %s scenario %s " % (last[0], last[1]), "%dg" % (spec["hold"] // 60 + 40)
    else:   # mpmenus: no flow; a fixed span of game time
        stopat, after = r"mp demo: done", "5"
    cap = max(3 * round(estimate_leg(dict(spec, hold=spec["hold"]), "high")), 1800)
    secs = cap if k != "mpmenus" else cap
    return {"stopat": stopat, "stopafter": after, "wall_cap_s": secs}

def fingerprint(table, specs):
    h = hashlib.sha256()
    h.update(json.dumps({"table": table, "legs": [(s["id"], s["stops"], s["env"], s["hold"]) for s in specs]}, sort_keys=True).encode())
    return h.hexdigest()[:16]

def write_leg(out, spec, a=None):
    d = out / "legs" / spec["id"]
    d.mkdir(parents=True, exist_ok=True)
    (d / "env.txt").write_text("# seedwalk leg %s (%s, %s): only allow-listed keys (plan.py check-env)\n" % (spec["id"], spec["kind"], spec["pass"])
                               + "".join(f"{k}={v}\n" for k, v in spec["env"]))
    if spec.get("input") == "input.txt":
        total = spec["hold"] if spec["kind"] in ("flow", "stage") else 2400
        (d / "input.txt").write_text(build_route(total, spec.get("route_variant", 0)))
    (d / "spec.json").write_text(json.dumps(spec, indent=1))

def cmd_make(args):
    a = {"out": None, "table": None, "passes": "p60:1.5,q30:1.0", "per_leg": 4, "hold": 5400, "hop": 2.0, "hop_max": 40, "enum": 0, "variant": 0,
         "harvest": True, "no_menus": False, "menus_both": False, "eboot": "", "seed": ""}
    it = iter(args)
    for x in it:
        if x == "--out": a["out"] = next(it)
        elif x == "--table": a["table"] = next(it)
        elif x == "--passes": a["passes"] = next(it)
        elif x == "--stops-per-leg": a["per_leg"] = int(next(it))
        elif x == "--hold": a["hold"] = int(next(it))
        elif x == "--hop": a["hop"] = float(next(it))
        elif x == "--hop-max": a["hop_max"] = int(next(it))
        elif x == "--enum": a["enum"] = int(next(it))
        elif x == "--variant": a["variant"] = int(next(it))
        elif x == "--no-harvest-switch": a["harvest"] = False
        elif x == "--no-menus": a["no_menus"] = True
        elif x == "--menus-both": a["menus_both"] = True
        elif x == "--seed": a["seed"] = next(it)
        else: raise SystemExit(f"plan.py make: unknown option {x}")
    if not a["out"]:
        raise SystemExit("plan.py make: --out OUT is required")
    if not 1 <= a["per_leg"] <= 24:
        raise SystemExit("plan.py make: --stops-per-leg must be 1..24")
    out = Path(a["out"]).resolve()
    table, specs = make_plan(a)
    errs = check_plan(table, specs)
    if errs:
        raise SystemExit("plan.py make: the plan is wrong:\n  " + "\n  ".join(errs))
    out.mkdir(parents=True, exist_ok=True)
    for s in specs:
        write_leg(out, s)
    plan = {"format": FORMAT, "fingerprint": fingerprint(table, specs), "table_entries": len(table), "table": table, "params": {k: v for k, v in a.items() if k != "out"},
            "legs": [s["id"] for s in specs], "harvest_key": HARVEST_KEY}
    (out / "plan.json").write_text(json.dumps(plan, indent=1))
    tot = sum(s["est_wall_s"] for s in specs)
    print(f"plan {plan['fingerprint']}: {len(table)} stops, {len(specs)} legs, typical wall {tot / 3600:.1f} h (plan.py estimate OUT) -> {out}")

def check_plan(table, specs):
    """every stage-table entry exactly once per pass among the flow/stage legs; every env passes the allow-list"""
    errs = []
    passes = sorted({s["pass"] for s in specs})
    for p in passes:
        got = [x for s in specs if s["pass"] == p and s["kind"] in ("flow", "stage") for x in s["stops"]]
        miss = [x for x in table if x not in got]
        dup = sorted({x for x in got if got.count(x) > 1})
        extra = [x for x in got if x not in table]
        if miss: errs.append(f"pass {p}: not covered: {miss[:5]}")
        if dup: errs.append(f"pass {p}: covered twice: {dup[:5]}")
        if extra: errs.append(f"pass {p}: not in the table: {extra[:5]}")
    ids = [s["id"] for s in specs]
    if len(set(ids)) != len(ids):
        errs.append("duplicate leg ids")
    for s in specs:
        for b in check_env_text("".join(f"{k}={v}\n" for k, v in s["env"])):
            errs.append(f"{s['id']}: {b}")
        if s["kind"] == "stage" and any(x.split(":")[0] not in NO_STAR_SELECT for x in s["stops"]):
            errs.append(f"{s['id']}: a stage leg holds a galaxy that has a star select")
        if s["kind"] == "flow" and any(x.split(":")[0] in NO_STAR_SELECT for x in s["stops"]):
            errs.append(f"{s['id']}: a flow leg holds a galaxy that has no star select")
    return errs

# ---- reading a leg's log ------------------------------------------------------------------------------------------------------------------
RE_ENTRY_DONE = re.compile(r"flow: entry (\d+) of (\d+) \(([^)]*)\) done")
RE_INSTEAD = re.compile(r"flow: entry (\d+) of (\d+): (\S+) instead of (\S+)")
RE_JUMP = re.compile(r"tour: jumping to (\S+) scenario (\d+) \((\d+) of the list\)")
RE_FLOWDONE = re.compile(r"flow: done: all (\d+) entries")
RE_COVER = re.compile(r"\bcover:\s*(.*)$")
RE_PROFILE = re.compile(r"\bprofile\b[^\n]*?\b([0-9a-f]{16})\b")
FATAL = re.compile(r"FATAL|HEAP FULL|did not compile|link failed|compile failed|a shader compile ran in play")

def read_text(p):
    try:
        return Path(p).read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""

def analyze(spec, log_text, status_text="", emu_text=""):
    """-> dict: which stops of the leg were finished, and what went wrong"""
    k, stops = spec["kind"], spec["stops"]
    done, bad = [], []
    lines = log_text.split("\n")
    if k in ("flow", "mpdemo", "menus"):
        got = {}
        for ln in lines:
            m = RE_ENTRY_DONE.search(ln)
            if m:
                got[int(m.group(1))] = m.group(3)
            m = RE_INSTEAD.search(ln)
            if m:
                bad.append(f"entry {m.group(1)}: {m.group(3)} instead of {m.group(4)}")
        for i, st in enumerate(stops, 1):
            if i in got:
                if got[i] == st.split(":")[0]:
                    done.append(st)
                else:
                    bad.append(f"entry {i}: finished in {got[i]}, asked {st}")
    elif k == "stage":
        jumped = {}
        for ln in lines:
            m = RE_JUMP.search(ln)
            if m:
                jumped[int(m.group(3))] = f"{m.group(1)}:{m.group(2)}"
        last_ok = "STOPAT matched" in status_text
        for i, st in enumerate(stops, 1):
            if jumped.get(i) == st and (((i + 1) in jumped) or (i == len(stops) and last_ok)):
                done.append(st)
            elif i in jumped and jumped[i] != st:
                bad.append(f"tour step {i}: {jumped[i]}, asked {st}")
    fatal = sorted({m.group(0) for ln in lines for m in [FATAL.search(ln)] if m})
    faults = emu_text.count("[invalid_access]")
    covers = [m.group(1) for ln in lines for m in [RE_COVER.search(ln)] if m]
    prof = sorted({m.group(1) for ln in lines if "profile" in ln for m in [RE_PROFILE.search(ln)] if m})
    finished = RE_FLOWDONE.search(log_text) is not None or "STOPAT matched" in status_text or "game seconds reached" in status_text
    if k == "mpmenus":
        done = []
        complete = finished and not bad
    else:
        complete = len(done) == len(stops) and not bad
    return {"complete": bool(complete), "stops_done": done, "stops_missing": [s for s in stops if s not in done], "problems": bad, "fatal_lines": fatal,
            "guest_faults": faults, "cover_lines": len(covers), "profile_words": prof, "run_finished": bool(finished), "status": status_text.strip()[:200]}

def cmd_leg_status(out, lid):
    out = Path(out)
    ld = out / "legs" / lid
    spec = json.loads((ld / "spec.json").read_text())
    r = ld / "run"
    res = analyze(spec, read_text(r / "log.txt"), read_text(r / "status.txt"), read_text(r / "vita3k.log"))
    res["leg"] = lid
    (ld / "status.json").write_text(json.dumps(res, indent=1))
    if res["complete"]:
        info = {}
        try:
            info = json.loads((ld / "run-info.json").read_text())
        except (OSError, ValueError):
            pass
        (ld / "DONE").write_text(json.dumps({"plan": json.loads((out / "plan.json").read_text())["fingerprint"], **info}))
        print(f"{lid}: complete ({len(res['stops_done'])}/{len(spec['stops'])} stops, {res['cover_lines']} cover lines)")
        return 0
    try:
        (ld / "DONE").unlink()
    except OSError:
        pass
    print(f"{lid}: INCOMPLETE: missing {res['stops_missing']}; problems {res['problems']}; fatal {res['fatal_lines']}")
    return 4

def cmd_resume(out):
    out = Path(out)
    plan = json.loads((out / "plan.json").read_text())
    made = []
    for lid in list(plan["legs"]):
        ld = out / "legs" / lid
        if (ld / "DONE").exists() or not (ld / "status.json").exists():
            continue
        # (harvprep31) one follow-up a leg: a second resume (run_all.sh calls it after every pass) does not make -r2 beside an -r1 that exists,
        # and a follow-up does not get one of its own (its stops are the ones the game could not finish twice: they are reported, not walked again)
        if re.search(r"-r\d+$", lid) or any(re.fullmatch(re.escape(lid) + r"-r\d+", x) for x in plan["legs"]):
            continue
        st = json.loads((ld / "status.json").read_text())
        spec = json.loads((ld / "spec.json").read_text())
        if st["complete"] or not st["stops_missing"] or spec["kind"] not in ("flow", "stage"):
            continue
        n = 1
        a = dict(plan["params"])
        a["hold"] = spec["hold"]
        ps = {"id": spec["pass"], "picture_mode": spec["picture_mode"], "zoom": spec["zoom"]}
        # a stop that did not finish because the game could not enter it is not asked again as the first of the leg: it goes last (a crash there costs the rest)
        miss = st["stops_missing"]
        ns = dict(spec, id=f"{lid}-r{n}", stops=miss, parent=lid, env=leg_env(spec["kind"], ps, miss, a))
        ns.update(stop_rule(ns))
        ns["est_wall_s"] = round(estimate_leg(ns))
        write_leg(out, ns)
        plan["legs"].append(ns["id"])
        made.append(ns["id"])
    (out / "plan.json").write_text(json.dumps(plan, indent=1))
    print("follow-up legs:", " ".join(made) if made else "none")
    return 0

def load_specs(out):
    out = Path(out)
    plan = json.loads((out / "plan.json").read_text())
    return plan, [json.loads((out / "legs" / l / "spec.json").read_text()) for l in plan["legs"]]

def cmd_list(out):
    plan, specs = load_specs(out)
    for s in specs:
        done = "DONE" if (Path(out) / "legs" / s["id"] / "DONE").exists() else "    "
        print(f"{s['id']:16s} {done} {s['kind']:8s} {s['pass']} {len(s['stops']):2d} stops  ~{s['est_wall_s'] / 60:5.0f} min  {s['stops'][0] if s['stops'] else '-'}{' .. ' + s['stops'][-1] if len(s['stops']) > 1 else ''}")

def cmd_estimate(out):
    plan, specs = load_specs(out)
    print(f"{'leg':16s} {'kind':8s} {'stops':>5s} {'low':>7s} {'typical':>8s} {'high':>7s}   (minutes of wall time, 2 shared cores, emulator speed {MODEL['low']['speed']}/{MODEL['typ']['speed']}/{MODEL['high']['speed']} game s per wall s)")
    tot = {"low": 0, "typ": 0, "high": 0}
    for s in specs:
        v = {w: estimate_leg(s, w) for w in tot}
        for w in tot: tot[w] += v[w]
        print(f"{s['id']:16s} {s['kind']:8s} {len(s['stops']):5d} {v['low'] / 60:7.0f} {v['typ'] / 60:8.0f} {v['high'] / 60:7.0f}")
    print(f"{'TOTAL':16s} {'':8s} {sum(len(s['stops']) for s in specs):5d} {tot['low'] / 3600:6.1f}h {tot['typ'] / 3600:7.1f}h {tot['high'] / 3600:6.1f}h   ({len(specs)} legs)")

def cmd_leginfo(out, lid):
    out = Path(out)
    s = json.loads((out / "legs" / lid / "spec.json").read_text())
    q = lambda v: "'" + str(v).replace("'", "'\\''") + "'"
    print(f"KIND={q(s['kind'])}\nBOOT={1 if s['boot'] else 0}\nINPUTKIND={q(s['input'] or '')}\nSTOPAT={q(s['stopat'])}\nSTOPAFTER={q(s['stopafter'])}\nWALLCAP={s['wall_cap_s']}\nNSTOPS={len(s['stops'])}")

def cmd_check_env(files):
    rc = 0
    for f in files:
        bad = check_env_text(read_text(f))
        if bad:
            rc = 3
            print(f"{f}: REFUSED")
            for b in bad: print("  " + b)
        else:
            print(f"{f}: ok")
    return rc

# ---- selftest -----------------------------------------------------------------------------------------------------------------------------
def selftest():
    import subprocess
    import tempfile
    n = [0, 0]
    def ok(cond, what):
        n[0] += 1
        if not cond:
            n[1] += 1
            print("FAIL:", what)
    # allow-list
    ok(check_env_text("GXV_VSOUT=0\n"), "GXV_VSOUT=0 refused")
    ok(check_env_text("GXV_VSOUT=1\n"), "GXV_VSOUT=1 refused too (no text switch may be set)")
    for k in text_switches():
        ok(check_env_text(f"{k}=1\n"), f"text switch {k} refused")
    for k in FORCED_PACK:
        ok(check_env_text(f"{k}=0\n"), f"forced pack {k} refused")
    ok(check_env_text("GXV_SHADERPACK=1\n"), "GXV_SHADERPACK=1 refused (no forced pack, even the default)")
    ok(check_env_text("GXV_FOO=1\n"), "unknown GXV key refused")
    ok(check_env_text("GXV_PICTURE_MODE=45\n"), "bad picture mode refused")
    ok(check_env_text("A=1\nA=2\n") != [], "duplicate refused")
    ok(check_env_text("GXV_SHADER_CACHE_TAG=a-b\n"), "bad tag refused")
    ok(check_env_text("GXV_STAGEWARM_FILE=/tmp/x\n"), "list path outside the card refused")
    ok(not check_env_text("# c\nPETARI_BOOT=flow\nPETARI_FLOW=A:1\nGXV_PICTURE_MODE=60\nGXV_SHADER_CACHE_TAG=sw31\nGXV_PACK_MISSLOG=1\nGXV_FIRSTUSE_LOG=1\n"
                          "GXV_STAGEWARM_FILE=ux0:data/smg/sw/stagewarm.bin\nSMG_MP_DEMO=menus\nGXV_SEEDWALK=1\nGXV_KEEP_RT=1\nGXV_COLSHADOW_LOG=1\n"), "the allowed set passes")
    ok(any("allow-list" in b for b in check_env_text("GXV_HRW=5\n")), "GXV_HRW not allowed")
    # the table, the real one
    table = read_table()
    ok(len(table) == len(set(table)) and len(table) >= 130, f"stage table {len(table)} entries")
    # splitting
    for per in (1, 3, 4, 5, 24):
        for tot in (1, 7, 24, 25, 120):
            ch = split(list(range(tot)), per)
            ok(sum(len(c) for c in ch) == tot and all(0 < len(c) <= per for c in ch), f"split {tot} by {per}")
    base = {"table": None, "passes": "p60:1.5,q30:1.0", "per_leg": 4, "hold": 5400, "hop": 2.0, "hop_max": 40, "enum": 0, "variant": 0, "harvest": True,
            "no_menus": False, "menus_both": False}
    for variant in (0, 1, 2):
        for per in (1, 4, 24):
            for passes in ("p60:1.5", "p60:1.5,q30:1.0", "q30:1.0,p60:1.0"):
                t, specs = make_plan(dict(base, variant=variant, per_leg=per, passes=passes))
                errs = check_plan(t, specs)
                ok(not errs, f"plan variant {variant} per {per} passes {passes}: {errs[:2]}")
                for p in {s['pass'] for s in specs}:
                    got = sorted(x for s in specs if s["pass"] == p and s["kind"] in ("flow", "stage") for x in s["stops"])
                    ok(got == sorted(t), f"every table entry once in pass {p}")
    t, specs = make_plan(dict(base))
    ok(all(len(s["stops"]) <= 4 for s in specs if s["kind"] in ("flow", "stage")), "legs of at most 4 stops")
    ok(specs[0]["kind"] == "menus", "the menus leg (makes the save template) comes first")
    ok(all(s["hold"] >= 120 + 40 * 2.0 * 60 for s in specs if s["kind"] in ("flow", "stage")), "hold covers the survey hops")
    # a broken plan is caught
    fl = [i for i, s in enumerate(specs) if s["kind"] == "flow"][0]
    broken = list(specs); broken[fl] = dict(specs[fl], stops=specs[fl]["stops"][1:])
    ok(check_plan(t, broken), "a missing stop is caught")
    broken = list(specs); broken[fl] = dict(specs[fl], stops=specs[fl]["stops"] + [specs[fl + 1]["stops"][0]])
    ok(check_plan(t, broken), "a duplicated stop is caught")
    broken = list(specs); broken[fl] = dict(specs[fl], env=specs[fl]["env"] + [("GXV_VSOUT", "0")])
    ok(check_plan(t, broken), "GXV_VSOUT=0 in a leg env is caught")
    # the scripted route
    txt = build_route(5400, 0); txt1 = build_route(5400, 1)
    ok(txt != txt1 and len(txt) > 500, "route variants differ")
    # the fixtures and the dry run
    fx = HERE / "fixtures"
    log = read_text(fx / "fake_flow_log.txt")
    spec = {"kind": "flow", "stops": ["HoneyBeeKingdomGalaxy:1", "EggStarGalaxy:1", "StarDustGalaxy:1"]}
    a1 = analyze(spec, log, "STOPAT matched after 100 s wall", "")
    ok(a1["complete"] and a1["cover_lines"] == 3 and a1["profile_words"] == ["0123456789abcdef"], f"fixture analysis {a1}")
    cut = "\n".join(log.split("\n")[:log.split("\n").index(next(l for l in log.split("\n") if "entry 3 of 3 (StarDustGalaxy) done" in l))])
    a2 = analyze(spec, cut, "", "")
    ok(not a2["complete"] and a2["stops_missing"] == ["StarDustGalaxy:1"], f"incomplete leg: {a2['stops_missing']}")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        out = td / "sw"
        run = lambda *c: subprocess.run([sys.executable, "-I", str(HERE / c[0])] + list(c[1:]), capture_output=True, text=True)
        r = run("plan.py", "make", "--out", str(out), "--passes", "p60:1.5", "--stops-per-leg", "24", "--no-menus")
        ok(r.returncode == 0, "make: " + r.stderr[-300:])
        plan = json.loads((out / "plan.json").read_text())
        ok(len(plan["legs"]) >= 2, "legs made")
        for lid in plan["legs"]:
            ok(run("plan.py", "check-env", str(out / "legs" / lid / "env.txt")).returncode == 0, f"{lid} env passes")
            ok((out / "legs" / lid / "input.txt").exists(), f"{lid} input.txt")
        # a dry run: three fake legs made from the fixtures, then collect
        r = run("plan.py", "make", "--out", str(td / "dry"), "--passes", "p60:1.5", "--stops-per-leg", "3", "--no-menus", "--table", str(fx / "fake_table.txt"))
        ok(r.returncode == 0, "make with the fake table: " + r.stderr[-300:])
        dry = td / "dry"
        plan = json.loads((dry / "plan.json").read_text())
        ok(plan["table_entries"] == 3 and len(plan["legs"]) == 1, f"fake table: {plan['legs']}")
        lid = plan["legs"][0]
        ld = dry / "legs" / lid
        (ld / "run").mkdir(); (ld / "card").mkdir()
        (ld / "run" / "log.txt").write_text(log)
        (ld / "run" / "status.txt").write_text("STOPAT matched (+3 s) after 100 s wall\n")
        (ld / "run" / "vita3k.log").write_text("x\n")
        (ld / "run" / "env.txt").write_text((ld / "env.txt").read_text())
        card = ld / "card"
        (card / "b4d4c2a9-sw31" / "v3" / "f" / "3A").mkdir(parents=True)
        (card / "b4d4c2a9-sw31" / "v3" / "f" / "3A" / "3A1B2C3D4E5F6071.gxp").write_bytes(b"GXPfake1")
        (card / "b4d4c2a9-sw31" / "v3" / "v" / "0F").mkdir(parents=True)
        (card / "b4d4c2a9-sw31" / "v3" / "v" / "0F" / "FF0123456789ABCD.gxp").write_bytes(b"GXPfake2")
        (card / "vgx_stagewarm.bin").write_bytes(b"VGXSTW01" + bytes(24 * 5))
        (card / "vgx_stagewarm.bin.jnl").write_bytes(b"VGXSTJ01" + bytes(8))   # (fix31: the journal goes to the harvest with the stage list)
        (card / "vgx_shaderlist.bin").write_bytes(b"VGXSHL01" + bytes(40))
        r = run("plan.py", "leg-status", str(dry), lid)
        ok(r.returncode == 0 and (ld / "DONE").exists(), "leg-status: complete -> DONE: " + r.stdout + r.stderr)
        r = run("collect.py", "collect", str(dry))
        ok(r.returncode == 0, "collect on the fixture: " + r.stdout[-400:] + r.stderr[-300:])
        h = dry / "harvest"
        man = json.loads((h / "MANIFEST.json").read_text()) if (h / "MANIFEST.json").exists() else {}
        ok(man.get("programs") == 2 and man.get("conflicts") == 0, f"harvest programs {man.get('programs')}")
        ok((h / "programs" / "b4d4c2a9" / "v3" / "f" / "3A" / "3A1B2C3D4E5F6071.gxp").exists(), "program in the harvest")
        ok(len(list((h / "lists").glob("*/vgx_stagewarm.bin"))) == 1, "stage list in the harvest")
        ok(len(list((h / "lists").glob("*/vgx_stagewarm.bin.jnl"))) == 1, "stage journal in the harvest")
        ok(man.get("coverage", {}).get("p60", {}).get("missing") == [], f"coverage {man.get('coverage')}")
        cov = (h / "cover.tsv").read_text() if (h / "cover.tsv").exists() else ""
        ok(cov.count("\n") == 4, "cover.tsv: header + 3 lines")
        # a conflicting file (same name, other bytes) in a second leg is reported
        ld2 = dry / "legs" / "p60-flow-99"
        (ld2 / "card" / "b4d4c2a9-sw31" / "v3" / "f" / "3A").mkdir(parents=True)
        (ld2 / "card" / "b4d4c2a9-sw31" / "v3" / "f" / "3A" / "3A1B2C3D4E5F6071.gxp").write_bytes(b"GXPOTHER")
        (ld2 / "run").mkdir()
        (ld2 / "spec.json").write_text(json.dumps({"id": "p60-flow-99", "kind": "flow", "pass": "p60", "stops": [], "env": [], "hold": 0}))
        (ld2 / "env.txt").write_text("GXV_PICTURE_MODE=60\n")
        plan["legs"].append("p60-flow-99"); (dry / "plan.json").write_text(json.dumps(plan))
        r = run("collect.py", "collect", str(dry))
        ok(r.returncode == 3 and "conflict" in r.stdout, "a conflicting program fails collect: " + r.stdout[-300:])
        # a leg env with a text switch fails collect
        (ld2 / "env.txt").write_text("GXV_VSOUT=0\n")
        (ld2 / "card" / "b4d4c2a9-sw31" / "v3" / "f" / "3A" / "3A1B2C3D4E5F6071.gxp").write_bytes(b"GXPfake1")
        r = run("collect.py", "collect", str(dry))
        ok(r.returncode == 3 and "REFUSED" in r.stdout, "a text switch in a leg's env fails collect: " + r.stdout[-300:])
        # resume
        (ld / "DONE").unlink()
        (ld / "run" / "log.txt").write_text(cut)
        (ld / "run" / "status.txt").write_text("exited rc=1 after 50 s wall\n")
        run("plan.py", "leg-status", str(dry), lid)
        plan["legs"].remove("p60-flow-99"); (dry / "plan.json").write_text(json.dumps(plan))
        r = run("plan.py", "resume", str(dry))
        ok(r.returncode == 0 and f"{lid}-r1" in r.stdout, "resume makes a follow-up leg: " + r.stdout)
        sp = json.loads((dry / "legs" / f"{lid}-r1" / "spec.json").read_text())
        ok(sp["stops"] == ["StarDustGalaxy:1"] and sp["parent"] == lid, f"follow-up stops {sp['stops']}")
        r = run("plan.py", "resume", str(dry))   # (harvprep31: run_all.sh resumes after every pass: no second follow-up for the same leg)
        ok(r.returncode == 0 and "none" in r.stdout and not (dry / "legs" / f"{lid}-r2").exists(), "a second resume makes no second follow-up: " + r.stdout)
        # leg.sh dry run (no emulator)
        env = dict(os.environ, DRY="1", EBOOT=str(td / "eboot.bin"), V3K=str(td / "v3k"))
        (td / "eboot.bin").write_bytes(b"x"); (td / "v3k").mkdir()
        (td / "v3k" / "run.sh").write_text("#!/bin/bash\necho SHOULD-NOT-RUN\n"); os.chmod(td / "v3k" / "run.sh", 0o755)
        (td / "v3k" / "pkgsc" / "shadercache").mkdir(parents=True)
        r = subprocess.run(["bash", str(HERE / "leg.sh"), str(dry), lid], capture_output=True, text=True, env=env)
        ok(r.returncode == 0 and "DRY" in r.stdout and "SHOULD-NOT-RUN" not in r.stdout, "leg.sh dry run: " + r.stdout[-400:] + r.stderr[-300:])
        r = subprocess.run(["bash", str(HERE / "leg.sh"), str(td / "v3k" / "in" / "x"), lid], capture_output=True, text=True, env=env)
        ok(r.returncode != 0 and "not a folder for harvest output" in r.stdout, "leg.sh refuses an output folder under the game data: " + r.stdout)
    print(f"selftest: {n[0] - n[1]} of {n[0]} checks passed")
    return 0 if n[1] == 0 else 1

def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__); return 0
    c, rest = argv[1], argv[2:]
    if c == "make": cmd_make(rest); return 0
    if c == "list": cmd_list(rest[0]); return 0
    if c == "estimate": cmd_estimate(rest[0]); return 0
    if c == "check-env": return cmd_check_env(rest)
    if c == "leginfo": cmd_leginfo(rest[0], rest[1]); return 0
    if c == "leg-status": return cmd_leg_status(rest[0], rest[1])
    if c == "resume": return cmd_resume(rest[0])
    if c == "selftest": return selftest()
    print(__doc__); return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))

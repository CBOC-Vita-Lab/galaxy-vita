#!/usr/bin/env python3
"""make_jobs.py: the relay jobs of the 3.1 console session (docs/CONSOLE-SESSION-3.1.md). Pure Python 3.8+, no network, no console: it only writes files.

  python3 tools/console31/make_jobs.py all  [--out docs/console31]   writes <out>/jobs/NNNN-*.json, <out>/staging/*.txt, <out>/JOBS.txt
  python3 tools/console31/make_jobs.py list [--out docs/console31]   prints JOBS.txt
  python3 tools/console31/make_jobs.py install --relay <.../relay>   copies staging/* (never env-user.txt) to relay/staging and the jobs to relay/claude-jobs-console31
  python3 tools/console31/make_jobs.py queue --relay <relay> <name>...   copies jobs/<name>.json into relay/inbox (refuses one that is there or ran)

Job format: the relay's own (a JSON "batch" of steps: put, launch, vc, shot, sleep, close, pull_logs, clear_input, ls, get, status), the same
as tools/pack24/make_jobs.py writes. The relay allows writes only to ux0:data/smg/*.txt and (eboot) the app, and never touches saves.

How a run is made. Every run replaces the WHOLE env.txt by the run's own file (the owner's lines are not appended), starts the game, waits, closes it,
pulls the logs and puts the owner's own env.txt back from relay/staging/env-user.txt (the owner makes that file from the backup, section 0 of the
guide). Runs go straight into a galaxy with PETARI_BOOT (test only: no menus, the save is read and never written) and, where the route matters,
play a scripted input (PETARI_SCRIPT_ANCHOR=play) made by tools/seedwalk/plan.py, so every arm plays the same route.
Kinds: U = unattended (a log verdict); O = the owner plays or watches (two jobs: <id>-start launches, <id>-end closes and pulls; queue the end job
only when the owner says so).
"""
import argparse, json, os, shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "seedwalk"))
sys.path.insert(0, str(REPO / "tools" / "pack24"))

BOOT_S, LOAD_S, END_S = 75, 45, 25      # launch to the first stage's start of load; one stage load; close + pull + restore
FIRST = 8100                            # job numbers 81xx..: this session; no other relay job of the last weeks uses them

def vc(c, w=1.0): return {"action": "vc", "cmd": c, "wait": w}
START = [{"action": "close"}, {"sleep": 3}, vc("screen on", 2), vc("unlock", 3), vc("nosleep on"), {"action": "clear_input"}]
END = [vc("release all", 1), {"action": "close"}, {"sleep": 4}, {"action": "pull_logs"}, {"action": "ls", "path": "ux0:data/smg"}, {"action": "clear_input"},
       {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}]
END_SCREEN = END + [vc("screen off", 1)]

STOP = {"obs": "AstroGalaxy:1", "honey": "HoneyBeeKingdomGalaxy:1", "beach": "HeavenlyBeachGalaxy:1", "dread": "CannonFleetGalaxy:1"}
STOPNAME = {"obs": "Observatory", "honey": "Honeyhive", "beach": "Beach Bowl", "dread": "Dreadnought"}

class Out:
    def __init__(self, out):
        self.out = out; self.jobs = out / "jobs"; self.stg = out / "staging"
        for d in (self.jobs, self.stg): d.mkdir(parents=True, exist_ok=True)
        self.rows = []; self.n = FIRST; self.routes = {}
    def num(self, step=1):
        v = self.n; self.n += step; return v
    def env(self, name, text):
        (self.stg / f"env-{name}.txt").write_text(text); return f"env-{name}.txt"
    def route(self, secs):
        name = f"input-c31-{secs}s.txt"
        if name not in self.routes:
            import plan
            (self.stg / name).write_text(plan.build_route(secs * 60)); self.routes[name] = secs
        return name
    def save(self, name, steps, secs, row):
        (self.jobs / f"{name}.json").write_text(json.dumps({"action": "batch", "steps": steps}))
        self.rows.append((name, secs, row))

def tour_env(title, stops, hold_s, route=False, quality=False, tag=None, bee=False, extra=()):
    L = [f"# {title}", "# console session 3.1: this file REPLACES env.txt for the run; the relay puts the owner's own env.txt back afterwards"]
    L += ["PETARI_BOOT=1", "PETARI_STAGE=" + ",".join(STOP[s] for s in stops), f"PETARI_STAGE_HOLD={hold_s * 60}", "PETARI_FLOW_LOG=1"]
    if route: L += ["PETARI_SCRIPT_ANCHOR=play", "PETARI_SCRIPT_LOG=1"]
    if bee: L += ["PETARI_POWERUP=4", "PETARI_POWERUP_AT=240", "PETARI_POWERUP_EVERY=300"]
    if quality: L += ["GXV_PICTURE_MODE=30"]
    if tag:
        L += [f"GXV_SHADER_CACHE_TAG={tag}", f"GXV_STAGEWARM_FILE=ux0:data/smg/{tag}-stagewarm.bin", f"GXV_SHADERLIST=ux0:data/smg/{tag}-shaderlist.bin"]
    L += list(extra)
    return "\n".join(L) + "\n"

def run_secs(stops, hold_s, last_s):
    return BOOT_S + LOAD_S * len(stops) + hold_s * (len(stops) - 1) + last_s + END_S

def steps_unattended(env, inp, total_s, shots=0, extra_put=()):
    st = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}]
    for src, dst in extra_put: st.append({"action": "put", "src": src, "path": dst})
    if inp: st.append({"action": "put", "src": inp, "path": "ux0:data/smg/input.txt"})
    st.append({"action": "launch"})
    wait = total_s - END_S
    if shots:
        gap = wait / (shots + 1)
        for k in range(shots): st += [{"sleep": round(gap, 1)}, {"action": "shot", "name": f"s{k + 1:02d}"}]
        st.append({"sleep": round(gap, 1)})
    else:
        while wait > 0.5:
            d = min(90.0, wait); st.append({"sleep": round(d, 1)}); wait -= d
    return st + list(END_SCREEN)

def add_U(o, tag_id, what, stops, hold_s, last_s, extra=(), route=True, quality=False, tag=None, bee=False, shots=0, extra_put=(), eyes="no"):
    n = o.num(); name = f"{n}-c31-{tag_id}"
    env = o.env(f"{n}-{tag_id}", tour_env(f"{n} {tag_id}: {what}", stops, hold_s, route, quality, tag, bee, extra))
    secs = run_secs(stops, hold_s, last_s)
    inp = o.route(max(hold_s, last_s)) if route else None
    o.save(name, steps_unattended(env, inp, secs, shots, extra_put), secs, {"kind": "U", "what": what, "eyes": eyes, "env": env})

def add_O(o, tag_id, what, stops, hold_s, play_s, extra=(), quality=False, bee=False, tag=None):
    n = o.num(); env = o.env(f"{n}-{tag_id}", tour_env(f"{n} {tag_id}: {what} (the owner plays or watches)", stops, hold_s, False, quality, tag, bee, extra))
    start = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}, {"action": "launch"}]
    secs = BOOT_S + LOAD_S * len(stops) + play_s + END_S
    o.save(f"{n}a-c31-{tag_id}-start", start, secs, {"kind": "O", "what": what + " [start: puts the env, launches; the run's whole time is shown]", "eyes": "yes", "env": env})
    o.save(f"{n}b-c31-{tag_id}-end", [vc("release all", 1), {"action": "close"}, {"sleep": 4}, {"action": "pull_logs"}, {"action": "ls", "path": "ux0:data/smg"},
                           {"action": "clear_input"}, {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}], 20,
           {"kind": "O", "what": what + " [end: queue only when the owner says done]", "eyes": "yes", "env": env})

def lines(*xs): return list(xs)

def build(o):
    # ---- 0. backups and the gate -------------------------------------------------------------------------------------------------------
    n = o.num(); o.save(f"{n}-c31-backup-list", [{"action": "status"}, {"action": "ls", "path": "ux0:data/smg"}, {"action": "pull_logs"}], 30,
                        {"kind": "U", "what": "section 0: list ux0:data/smg and pull log*, env*, settings* (this is the env.txt backup)", "eyes": "no"})
    n = o.num(); o.save(f"{n}-c31-backup-settings", [{"action": "get", "path": "ux0:data/smg/player-settings.cfg"}], 15,
                        {"kind": "U", "what": "section 0: copy player-settings.cfg (fails if the file does not exist: then there is nothing to back up)", "eyes": "no"})
    # one pull job per checkpoint of block 1 (the owner plays by hand; the relay only pulls logs)
    for k in range(1, 7):
        n = o.num(); o.save(f"{n}-c31-pull-b1-{k}", [{"action": "pull_logs"}, {"action": "ls", "path": "ux0:data/smg"}], 20,
                            {"kind": "U", "what": f"block 1 checkpoint {k}: pull the logs and list the data folder (the game may stay open)", "eyes": "no"})
    o.n = FIRST + 8
    n = o.num(); o.save(f"{n}-c31-eboot-hash", [{"action": "get", "path": "ux0:app/SMGV00001/eboot.bin"}], 60,
                        {"kind": "U", "what": "section 0: copy the installed eboot; result.json gives its SHA-256 (compare with the build's BUILD-INFO.txt)", "eyes": "no"})
    o.n = FIRST + 20
    add_U(o, "probe", "gate: Observatory 60 s, Performance, no switch (is PETARI_BOOT working, which picture mode, are the census lines there)", ["obs"], 60, 60, route=False, shots=1)
    add_U(o, "K0a", "control, Observatory then Honeyhive, the scripted route, no A/B line (texture-upload census and cover lines come from it)", ["obs", "honey"], 60, 120)
    # ---- A/B: the owner's card as it is ----------------------------------------------------------------------------------------------------
    C182 = lines("GXV_HRW41_DEPTH=1", "GXV_HRW41_BOXKEY=1", "GXV_HRW41_BOX_ROUND=1", "GXV_HRW41_BUDGET=1", "GXV_HRW_STAGE_AUTO_RETRY=3")
    add_U(o, "A1-c182", "C182 smoothing set, Observatory then Honeyhive, the scripted route", ["obs", "honey"], 60, 120, extra=C182)
    add_O(o, "A1o-on", "C182 smoothing set, owner plays: jumps and fast camera turns", ["obs", "honey"], 120, 240, extra=C182)
    add_O(o, "A1o-off", "control for A1o, owner plays the same route", ["obs", "honey"], 120, 240)
    # V0-V6: the owner plays Honeyhive (bee suit, lily pond) for 150 s, then the Observatory
    V = [("V0", "baseline (census only)", []), ("V4", "GXV_POSE_PHASE=1", ["GXV_POSE_PHASE=1"]), ("V3", "GXV_HRW=0 (no second picture)", ["GXV_HRW=0"]),
         ("V1", "GXV_HR_PLANTMOVE=0", ["GXV_HR_PLANTMOVE=0"]), ("V2", "GXV_FARLOGIC_DRY=1", ["GXV_FARLOGIC_DRY=1"]), ("V5", "GXV_POSE_PHASE=2", ["GXV_POSE_PHASE=2"]),
         ("V6", "only after V3: the depth-edge option (needs the depth mask)", ["GXV_HRW41_DEPTH=1", "GXV_HRW41_DEPTH_EDGE=1"]),
         ("V0q", "only if the owner saw the vibration in Quality: baseline in Quality (GXV_PICTURE_MODE=30)", []),
         ("V4q", "only if the owner saw the vibration in Quality: GXV_POSE_PHASE=1 in Quality", ["GXV_POSE_PHASE=1"])]
    for vid, what, ex in V:
        add_O(o, vid, f"pose phase {vid}: {what}; Honeyhive bee suit 150 s, then the Observatory", ["honey", "obs"], 150, 240, extra=["GXV_POSE_PHASE_CENSUS=1"] + ex, bee=True,
              quality=vid.endswith("q"))
    add_U(o, "A2-la88", "compact generated mipmaps, Observatory then Honeyhive", ["obs", "honey"], 60, 120, extra=["GXV_LA88_MIPS=1"], eyes="optional")
    # P0-P6: pop-in census, scripted
    PC = ["GXV_POPIN_CENSUS=1"]
    P = [("P0", "defaults, census only", PC, ()),
         ("P1", "the game's own pops: budget off, governors pinned, presets file", PC + ["GXV_PERF_BUDGET=0", "GXV_CPULOD=0", "GXV_FE_GOV=0", "GXV_GAME_GOV=0", "GXV_CPUGOV=0",
               "GXV_STAGE_PRESETS_FILE=ux0:data/smg/popin31-p1-presets.txt"], [("popin31-p1-presets.txt", "ux0:data/smg/popin31-p1-presets.txt")]),
         ("P2", "flights exact (the intro fly-in and the flights: GXV_PB_DEMO=2, the cheapest arm with the most to show; run it first)", PC + ["GXV_PB_DEMO=2"], ()),
         ("P3", "flights exact + invisibility floor 3 px", PC + ["GXV_PB_DEMO=2", "GXV_PB_MINPX=3"], ()),
         ("P4", "flights exact + floor 6 px", PC + ["GXV_PB_DEMO=2", "GXV_PB_MINPX=6"], ()),
         ("P5", "optional: floor 3 px with lead 6", PC + ["GXV_PB_DEMO=2", "GXV_PB_MINPX=3", "GXV_POPIN_LEAD=6"], ()),
         ("P6", "plant reveal off (the owner keeps it on until this says otherwise)", PC + ["GXV_HONEY_PLANT_REVEAL=0"], ())]
    (o.stg / "popin31-p1-presets.txt").write_text(
        "# popin31 P1: the base class without the budget's drawn cuts and without cpulod (sizes, fx and clip_skipempty as shipped)\n"
        "@class base start=6 floor=6 ceil=6 size=fixed fx=down clip_skipempty=1\nAstroGalaxy class=base\nAstroDome class=base\n")
    for pid, what, ex, put in P:
        add_U(o, pid, f"pop-in {pid}: {what}; Honeyhive 200 s (the fly-in first) then Beach Bowl 80 s", ["honey", "beach"], 200, 80, extra=ex, extra_put=put, eyes="optional")
    # the 30 cap rule (setnat31) and the picture-size gate (s1117b)
    add_U(o, "F30", "env GXV_FPS_LIMIT=30 under Smooth Motion: the fixed 30 cap of c181 (setnat31's fix); Honeyhive 90 s", ["honey"], 90, 90, extra=["GXV_FPS_LIMIT=30"])
    add_U(o, "G-ctl", "picture-size gate control: Beach Bowl 150 s, Performance, defaults (gate on)", ["beach"], 150, 150)
    add_U(o, "G-off", "picture-size gate off: GXV_AUTOQ_GPUGATE=0, same route", ["beach"], 150, 150, extra=["GXV_AUTOQ_GPUGATE=0"])
    # ---- cold card (a tagged shader cache of its own: the owner's cache and lists are neither read nor written) -----------------------------------
    add_U(o, "K1-cold", "cold-card control (tag c31k): Observatory then Honeyhive", ["obs", "honey"], 60, 120, tag="c31k")
    add_U(o, "A3-hold", "C185 unknown-VRAM hold (tag c31h): Observatory then Honeyhive", ["obs", "honey"], 60, 120, tag="c31h", extra=["GXV_C185_HOLD_UNKNOWN_VRAM=1"])
    # scenario order: learn, then measure, with the switch on (tag c31s) and off (tag c31t); stops AstroGalaxy:1,:3 and Honeyhive:1
    for tg, val in (("c31s", 1), ("c31t", 0)):
        for ph in ("learn", "measure"):
            n = o.num(); name = f"{n}-c31-S{val}-{ph}"
            env = o.env(f"{n}-S{val}-{ph}", "\n".join([f"# {n}: scenario hints {'on' if val else 'off'}, {ph} run (tag {tg})", "PETARI_BOOT=1",
                  "PETARI_STAGE=AstroGalaxy:1,AstroGalaxy:3,HoneyBeeKingdomGalaxy:1" if ph == "learn" else "PETARI_STAGE=AstroGalaxy:3,AstroGalaxy:1,HoneyBeeKingdomGalaxy:1",
                  "PETARI_STAGE_HOLD=3600", "PETARI_FLOW_LOG=1", f"GXV_SHADER_CACHE_TAG={tg}", f"GXV_STAGEWARM_FILE=ux0:data/smg/{tg}-stagewarm.bin",
                  f"GXV_SHADERLIST=ux0:data/smg/{tg}-shaderlist.bin", f"GXV_STAGEQ_SCENARIO={val}"]) + "\n")
            secs = BOOT_S + 3 * LOAD_S + 2 * 60 + 60 + END_S
            o.save(name, steps_unattended(env, None, secs), secs, {"kind": "U", "what": f"scenario order, hints {'on' if val else 'off'}, {ph}", "eyes": "no", "env": env})
    # ---- CPU switches (perf-inventory C-off18): verify all six in one run, then each alone at Beach Bowl in Quality ----------------------------------
    # (fixnow31, emuverify31's verdicts: REPLAY25, RT_INST, STARBATCH and PAR25 are ready for the console; IDLE25 =2 showed 12,025 of 251,372 mismatches in Vita3K (the ALOD
    #  half-speed turns) and NOOP_FILTER has no readable counter: both are HELD, not in the verify run, and their A/B jobs below say so)
    VER = ["GXV_REPLAY25=2", "GXV_RT_INST=2", "GXV_STARBATCH=2", "GXV_PAR25=2", "GXV_WORKLOAD_PROBE=1"]
    add_U(o, "CPU-verify", "four CPU switches in verify mode together (replay25, rt-inst, starbatch, par25 observe) with the workload probe; read 0 differed / 0 mismatches; Quality; Honeyhive then Beach Bowl",
          ["honey", "beach"], 120, 120, extra=VER + ["GXV_TURN_PROF=1"], quality=True)
    add_U(o, "CPU-ctl", "CPU control: Beach Bowl 150 s, Quality, turn profile on", ["beach"], 150, 150, extra=["GXV_TURN_PROF=1"], quality=True)
    for k, line in (("idle25", "GXV_IDLE25=1"), ("replay25", "GXV_REPLAY25=1"), ("par25", "GXV_PAR25=1"), ("noop", "GXV_NOOP_FILTER=1"), ("rtinst", "GXV_RT_INST=1"), ("starbatch", "GXV_STARBATCH=1")):
        held = k in ("idle25", "noop")
        add_U(o, f"CPU-{k}", ("HELD, do not queue (emuverify31: " + ("IDLE25 verify mismatched in Vita3K" if k == "idle25" else "no readable NOOP_FILTER counter") + "): ") + f"CPU A/B {line}: Beach Bowl 150 s, Quality, turn profile on"
              if held else f"CPU A/B {line}: Beach Bowl 150 s, Quality, turn profile on", ["beach"], 150, 150, extra=["GXV_TURN_PROF=1", line], quality=True)
    # DMA, clock, ties
    for k, v, w in (("A", 3, "census: the CPU copies, timed"), ("B", 2, "verify: must say 0 differed"), ("C", 1, "the engine")):
        add_U(o, f"DMA-{k}", f"GXV_DMA_LOAD={v} ({w}); Observatory then Honeyhive, hold 2400 turns", ["obs", "honey"], 40, 60, extra=[f"GXV_DMA_LOAD={v}"], route=False)
    add_U(o, "CLOCK", "GXV_CLOCK_CENSUS=1: Observatory 120 s, Honeyhive (fly-in + 120 s)", ["obs", "honey"], 120, 200, extra=["GXV_CLOCK_CENSUS=1"])
    add_U(o, "HSR-census", "Dreadnought ties census (no change to the picture): stand still 240 s", ["dread"], 240, 240, route=False,
          extra=["GXV_HSR_ORDER=2", "GXV_HSR_ORDER_TIES=4", "GXV_HSR_GEO_BUDGET=30000", "GXV_HSR_GEO_KEEP_MB=24"])
    add_U(o, "HSR-gain", "only if the census frees a draw: ties 4 on (exact), 240 s", ["dread"], 240, 240, route=False,
          extra=["GXV_HSR_ORDER=1", "GXV_HSR_ORDER_TIES=4", "GXV_HSR_GEO_BUDGET=30000", "GXV_HSR_GEO_KEEP_MB=24"])
    add_U(o, "HSR-ctl", "control for HSR-gain: Dreadnought, nothing, 240 s", ["dread"], 240, 240, route=False)
    add_U(o, "BEPROF", "optional: GXV_BE_PROF=4 per-draw uniform figures, Beach Bowl 120 s", ["beach"], 120, 120, extra=["GXV_BE_PROF=4"])
    add_U(o, "K0b", "control again at the end (drift): same as K0a", ["obs", "honey"], 60, 120)
    # popin31b: the floor by kind, every arm with flights exact (GXV_PB_DEMO=2, which goes on first), and the exact cheap give-backs (numbers 8170..)
    o.n = FIRST + 70
    PK = PC + ["GXV_PB_DEMO=2"]
    for pid, what, ex in (("P7", "the shadows' floor only, 3 px (GXV_PB_MINPX_SHADOW=3)", ["GXV_PB_MINPX_SHADOW=3"]),
                          ("P8", "the plants' floor only, 3 px (GXV_PB_MINPX_DECOR=3)", ["GXV_PB_MINPX_DECOR=3"]),
                          ("P9", "P8 + the plants' members drawn as one GL draw (GXV_STATIC_INST=1, exact)", ["GXV_PB_MINPX_DECOR=3", "GXV_STATIC_INST=1"]),
                          ("P10", "verify, optional: shadows and plants 3 px with the shadow-memo and gravity-memo verify modes (0 mismatches)",
                           ["GXV_PB_MINPX_SHADOW=3", "GXV_PB_MINPX_DECOR=3", "GXV_SHADOWMEMO_VERIFY=1", "GXV_MO_GRAVMEMO=2"])):
        add_U(o, pid, f"pop-in {pid}: {what}; flights exact; Honeyhive 200 s (the fly-in first) then Beach Bowl 80 s", ["honey", "beach"], 200, 80, extra=PK + ex,
              eyes="optional")
    # ---- fixnow31: the four arms of fable's second review, crisp31's arms and emuverify31's combined run (numbers 8180..; the older numbers stay as they are) --------------
    o.n = FIRST + 80
    FLIP = ["GXV_HRW_FLIPBOOK=240", "GXV_HRW_FLIPBOOK_FROM=600"]   # (keepdepth31: half-size PPMs of every picture in ux0:data/smg/hrw31/; clear the folder between arms)
    for kid, what, ex in (("KD0", "keepdepth K0: GXV_HRW_KEEPDEPTH=0 (c181 as it was; Mario never hidden in B)", ["GXV_HRW_KEEPDEPTH=0"]),
                          ("KD2", "keepdepth K2: GXV_HRW_KEEPDEPTH=2 (census only, pixels as K0; read the 'hrw: keepdepth31' line)", ["GXV_HRW_KEEPDEPTH=2"]),
                          ("KD1", "keepdepth K1: the default (clear left out; read violations, which must be 0, and 'cleared for')", [])):
        add_O(o, kid, what + "; Honeyhive, Performance + 60 Assist, the route of keepdepth31 (grass, posts, a fast pan, a wall jump)", ["honey"], 120, 120, extra=FLIP + ex)
    add_O(o, "FUR-A", "fur A: the default (GXV_PB_FUR_SURFACE on) with GXV_POPIN_LOG=1 GXV_FURCENSUS=1; Honeyhive: climb the Queen from the ground to her head and back (2 min), then orbit her at 2000-3000 units; note GPU ms and the governor level",
          ["honey"], 120, 240, extra=["GXV_POPIN_LOG=1", "GXV_FURCENSUS=1"])
    add_O(o, "FUR-C", "fur C: GXV_PB_FUR_SURFACE=0 (the old origin rule, the baseline), same lines and same climb as FUR-A", ["honey"], 120, 240,
          extra=["GXV_POPIN_LOG=1", "GXV_FURCENSUS=1", "GXV_PB_FUR_SURFACE=0"])
    add_U(o, "GATE-ctl", "GPU gate control (default GXV_AUTOQ_GPUGATE=1) on a GPU-heavy route: Beach Bowl 150 s, Performance; compare shed steps and shown fps with GATE-off", ["beach"], 150, 150)
    add_U(o, "GATE-off", "GXV_AUTOQ_GPUGATE=0 (c181's rule), same route as GATE-ctl: do the shed steps and shown fps differ", ["beach"], 150, 150, extra=["GXV_AUTOQ_GPUGATE=0"])
    add_U(o, "BF-0", "blendfix timing A: GXV_POSE_BLENDFIX=0, census off; Beach Bowl (penguins) then the Observatory (Lumas), scripted; compare fps: lines and pair p95 with BF-1", ["beach", "obs"], 90, 90,
          extra=["GXV_POSE_BLENDFIX=0"])
    add_U(o, "BF-1", "blendfix timing B: GXV_POSE_BLENDFIX=1 (the default), census off; same route as BF-0. Expected: no difference beyond noise", ["beach", "obs"], 90, 90)
    # crisp31 (reports/crisp31.md): Performance W0/W1, Quality Q0..Q4; the owner looks (flip-book pairs, screenshots at the same spot)
    add_O(o, "W0", "crisp31 W0: control (none); Performance, 60 Assist on; Honeyhive pan over fine ground texture, Observatory railings; flip-book pairs", ["honey", "obs"], 120, 180, extra=FLIP)
    add_O(o, "W1", "crisp31 W1: GXV_HRW_WARP_NEAREST=1; same route: is the second picture as sharp as the first, any half-pixel shimmer on thin edges; log 'warps with point reads' > 0", ["honey", "obs"], 120, 180,
          extra=FLIP + ["GXV_HRW_WARP_NEAREST=1"])
    add_O(o, "Q0", "crisp31 Q0: control (none); Quality, Dreadnought far zoom (or Kingfin): fps and late pictures", ["dread"], 120, 180, quality=True)
    add_O(o, "Q1", "crisp31 Q1: GXV_Q30_FULLFX=1; Quality, same place as Q0: log has the '[crisp31] GXV_Q30_FULLFX=1' line and no 'half-size effects and blur on'; may lose some pictures where Q0 shed the effects", ["dread"], 120, 180,
          quality=True, extra=["GXV_Q30_FULLFX=1"])
    add_O(o, "Q2", "crisp31 Q2: GXV_EDGE_DEPTH=1; Quality at 1:1: screenshots of Mario's eyes, emblems, sign text, a fence or leaf silhouette ('[aa25]' line)", ["honey"], 120, 150, quality=True, extra=["GXV_EDGE_DEPTH=1"])
    add_O(o, "Q3", "crisp31 Q3: GXV_PASSHALF=0; Quality, Beach Bowl under water or Kingfin: is the glow less blocky, and the GPU ms", ["beach"], 120, 150, quality=True, extra=["GXV_PASSHALF=0"])
    add_O(o, "Q4", "crisp31 Q4: GXV_Q30_MID=0; Quality in a stage that took step 6 (a 'q30lock: ... 896x512' line in Q0): 960x544 kept, fps against Q0", ["beach"], 120, 150, quality=True, extra=["GXV_Q30_MID=0"])
    # emuverify31's combined =1 run (four exact switches together, after the one-at-a-time arms above)
    add_U(o, "CPU-all4", "CPU A/B, the four exact switches together: GXV_REPLAY25=1 GXV_RT_INST=1 GXV_STARBATCH=1 GXV_PAR25=1 (GXV_PAR_DL and GXV_STARBATCH_MIN stay at their defaults); Beach Bowl 150 s, Quality, turn profile on",
          ["beach"], 150, 150, extra=["GXV_TURN_PROF=1", "GXV_REPLAY25=1", "GXV_RT_INST=1", "GXV_STARBATCH=1", "GXV_PAR25=1"], quality=True)
    # q30cush31: Quality's odd stutter, three candidates apart (plan-cpu31 3.1: Q1 late pictures, Q2 pose rhythm, Q3 no cushion). Every arm is Quality (GXV_PICTURE_MODE=30 via quality=True), the same
    # scripted route (Honeyhive, then the Beach Bowl heavy stretch), the "q30sep:" line (a: late pictures, c: drain holds and near misses, b: the pose census) and the turn profile on.
    o.n = FIRST + 100
    QS = ["GXV_Q30_SEP=1", "GXV_POSE_PHASE_CENSUS=1", "GXV_TURN_PROF=1"]
    for qid, what, ex in (("QS0", "baseline with the censuses (read: q30sep late / drain / pose lines, smooth: holds 50/67+)", []),
                          ("QS1", "GXV_POSE_PHASE=1 (the pose rhythm candidate: pose age at draw 0)", ["GXV_POSE_PHASE=1"]),
                          ("QS2", "GXV_Q30_CUSHION=1 (the margin cushion: +1 vblank of latency while on; read holds avoided and gave up)", ["GXV_Q30_CUSHION=1"]),
                          ("QS3", "GXV_POSE_PHASE=1 and GXV_Q30_CUSHION=1 together", ["GXV_POSE_PHASE=1", "GXV_Q30_CUSHION=1"]),
                          ("QS4", "optional: GXV_Q30_DRAIN=0 (the model's S1 chain says the drain re-thins a healed margin; skip if Q0 shows no drain holds)", ["GXV_Q30_DRAIN=0"])):
        add_U(o, qid, f"Quality stutter {qid}: {what}; Honeyhive 120 s, then Beach Bowl 180 s", ["honey", "beach"], 120, 180, extra=QS + ex, quality=True, eyes="optional" if qid != "QS0" else "no")
    # midflush31 (docs/CONSOLE-SESSION-3.1.md section 3.13): the mid-pass split (GXV_MIDFLUSH, off by default) and the sceGxm ring sizes (one arm, three keys). Every arm has the same census
    # lines ("midflush:", "ring-5s:", be-split with every draw timed, gpu-scenes) so the controls are read the same way; the same scripted route, Performance, Observatory then Beach Bowl.
    o.n = FIRST + 110
    MF = ["GXV_RING_CENSUS=1", "GXV_BE_SPLIT=2", "GXV_GPU_SCENES=1"]
    for mid, what, ex in (("MF-0", "control: census only, GXV_MIDFLUSH off", []),
                          ("MF-1", "GXV_MIDFLUSH=64 (a flush every 64 EFB draws, wherever that is; the old switch, sh3vita's N)", ["GXV_MIDFLUSH=64"]),
                          ("MF-0b", "second control (A/B alternation: off, on, off): same as MF-0", []),
                          ("MF-2", "GXV_MIDFLUSH=10064 (after 64 draws, at the next change of program)", ["GXV_MIDFLUSH=10064"]),
                          ("MF-3", "GXV_MIDFLUSH=30003 (3 flushes in the largest EFB scene, sized from the picture before, at a change of program)", ["GXV_MIDFLUSH=30003"]),
                          ("RING-1", "sceGxm rings: GXV_VITA_VDM_KIB=1024 GXV_VITA_FRAGMENT_KIB=1024 GXV_VITA_USSE_KIB=1024 (sh3vita's sizes; USSE at our clamp)",
                           ["GXV_VITA_VDM_KIB=1024", "GXV_VITA_FRAGMENT_KIB=1024", "GXV_VITA_USSE_KIB=1024"])):
        add_U(o, mid, f"mid-pass split {mid}: {what}; Observatory 90 s then Beach Bowl 90 s, Performance, scripted route", ["obs", "beach"], 90, 90, extra=MF + ex)
    # armaudit31 (18 Nov, reports/armaudit31.md): every switch that waits for a console A/B and had no arm. Numbers 8220.. (none of them is used above). Performance-only arms
    # need Performance with 60 Assist on (console5); Quality-only arms say so. The unattended ones judge by log lines; the owner-watched ones (O) are the ones that need eyes.
    o.n = FIRST + 120
    # warpsteady31 (reports/warpsteady31.md): plane ramp, 30-hold, edge limit; Performance + 60 Assist, the plane on as in the report, the same scripted route for every arm
    WS = ["GXV_HRW36_PLANE=1", "GXV_HRW36_PLANE_R=500"]
    for wid, what, ex in (("S0", "the control: plane on, nothing else", []),
                          ("S1", "GXV_HRW_PLANE_RAMP=4 (the plane's weight steps by 1/4 a pair: no flip of the near ground)", ["GXV_HRW_PLANE_RAMP=4"]),
                          ("S2", "GXV_HRW_PLANE_RAMP=6 (a slower ramp)", ["GXV_HRW_PLANE_RAMP=6"]),
                          ("S3", "GXV_HRW_HOLD30_MS=500 (after a give-up, 30 for half a second, then 60 again)", ["GXV_HRW_HOLD30_MS=500"]),
                          ("S4", "GXV_HRW_HOLD30_MS=300 (a shorter hold)", ["GXV_HRW_HOLD30_MS=300"]),
                          ("S5", "GXV_HRW_PLANE_RAMP=4 and GXV_HRW_HOLD30_MS=500 together", ["GXV_HRW_PLANE_RAMP=4", "GXV_HRW_HOLD30_MS=500"]),
                          ("E1", "GXV_HRW_EDGE_PX=24 (the stale-comment value: more 30 on pans, less smear)", ["GXV_HRW_EDGE_PX=24"]),
                          ("E2", "GXV_HRW_EDGE_PX=64 (the owner's 15 Nov trial value: fewer drops, more smear)", ["GXV_HRW_EDGE_PX=64"]),
                          ("E3", "GXV_HRW_EDGE_MIRROR=1 (smear of the edge texel against mirrored texels)", ["GXV_HRW_EDGE_MIRROR=1"])):
        add_U(o, wid, f"warpsteady31 {wid}, Performance + 60 Assist: {what}; Honeyhive 120 s then Beach Bowl 90 s, scripted; read the hrw: pictures / warp edge / warpsteady31 lines", ["honey", "beach"], 120, 90,
              extra=WS + ex, eyes="optional")
    # keepdepth31 / emufix31: the full-size box key, its control, and the 2D-writes option (after KD2's census says there is something to keep)
    for kid, what, ex in (("KD-B1", "GXV_HRW_BOXDEPTH=1 (the player box keyed on the full-size depth; read 'boxes keyed on the full-size depth' near the box count and 'no depth texture' 0)", ["GXV_HRW_BOXDEPTH=1"]),
                          ("KD-B1off", "GXV_HRW_BOXDEPTH=1 and GXV_HRW_KEEPDEPTH=0 (control: every box falls back, 'no kept depth' = pairs; pictures as KEEPDEPTH=0)", ["GXV_HRW_BOXDEPTH=1", "GXV_HRW_KEEPDEPTH=0"]),
                          ("KD-2D", "GXV_HRW_KEEPDEPTH_2D=1 (keep a first picture whose later depth events are partial 2D draw writes; NOT exact: 'cleared for N (inexact, 2D writes not restored: M)', M must be 0; run after KD2's census shows 'refused only by partial draw writes' over 0)", ["GXV_HRW_KEEPDEPTH_2D=1"])):
        add_U(o, kid, f"keepdepth31, Performance + 60 Assist: {what}; Honeyhive 120 s then Beach Bowl 90 s, scripted (control: S0)", ["honey", "beach"], 120, 90, extra=WS + ex, eyes="optional")
    # popin31b / fable-tester31: the floor of the kinds the first arms left out, the handheld zoom, the hysteresis (every arm flights exact like P7-P9; control P2)
    for pid, what, ex in (("P11", "the fur's floor only, 3 px (GXV_PB_MINPX_FUR=3)", ["GXV_PB_MINPX_FUR=3"]),
                          ("P12", "the model swaps' floor only, 3 px (GXV_PB_MINPX_LOD=3)", ["GXV_PB_MINPX_LOD=3"]),
                          ("P13", "the far specks' floor only, 3 px (GXV_PB_MINPX_SPECK=3)", ["GXV_PB_MINPX_SPECK=3"]),
                          ("P14", "the thin star-bit pieces' floor only, 3 px (GXV_PB_MINPX_STARBIT=3)", ["GXV_PB_MINPX_STARBIT=3"]),
                          ("P15", "the particles' floor only, 3 px (GXV_PB_MINPX_PARTICLE=3)", ["GXV_PB_MINPX_PARTICLE=3"]),
                          ("P16", "the game's own clip distances grow with the handheld zoom, enemies (GXV_POPIN_ZOOM=2; changes the original distances: its price)", ["GXV_POPIN_ZOOM=2"]),
                          ("P17", "the port's distances grow with the handheld zoom (GXV_POPIN_ZOOM=1; undoes a port-made worsening)", ["GXV_POPIN_ZOOM=1"]),
                          ("P18", "hysteresis at the limit (GXV_POPIN_HYST=8): things stop flipping at the cut", ["GXV_POPIN_HYST=8"])):
        add_U(o, pid, f"pop-in {pid}: {what}; flights exact; Honeyhive 200 s (the fly-in first) then Beach Bowl 80 s", ["honey", "beach"], 200, 80, extra=PK + ex, eyes="optional")
    # bowl31 / ondraw31: pose made only where it is drawn (Quality; Beach Bowl bay, then Honeyhive for the enemy and NPC kinds). V = verify, then off / on / off.
    QT = ["GXV_TURN_PROF=1", "GXV_POSE_PHASE_CENSUS=1"]
    for bid, what, ex in (("BP-V", "GXV_BOWL_POSE=2 (verify: fish 'would leave out' over 0, mismatches 0/0/0/0, 'stale at draw' 0, no 'bowl-pose: MISMATCH')", ["GXV_BOWL_POSE=2"]),
                          ("BP-0", "GXV_BOWL_POSE=0 (control)", ["GXV_BOWL_POSE=0"]),
                          ("BP-1", "GXV_BOWL_POSE=1 (the fish pose made only on the drawn turn: 'LATE reads' 0, 'stale at draw' 0)", ["GXV_BOWL_POSE=1"]),
                          ("BP-0b", "GXV_BOWL_POSE=0 (second control, off-on-off)", ["GXV_BOWL_POSE=0"])):
        add_U(o, bid, f"bowl31 {bid}, Quality: {what}; Beach Bowl 180 s (the bay with the fish schools), scripted", ["beach"], 180, 180, extra=QT + ex, quality=True, eyes="optional")
    o.n = FIRST + 144
    for oid, what, ex in (("OD-V", "GXV_POSE_ONDRAW=2 (verify: 'would leave out' over 0 for several kinds, mismatches 0/0/0/0, 'stale at draw' 0, no 'pose-ondraw: MISMATCH')", ["GXV_POSE_ONDRAW=2"]),
                          ("OD-0", "GXV_POSE_ONDRAW=0 (control)", ["GXV_POSE_ONDRAW=0"]),
                          ("OD-1", "GXV_POSE_ONDRAW=1 (every class's pose only on the drawn turn when nothing reads it: 'LATE reads' 0, 'drift' 0, 'stale at draw' 0)", ["GXV_POSE_ONDRAW=1"]),
                          ("OD-0b", "GXV_POSE_ONDRAW=0 (second control, off-on-off)", ["GXV_POSE_ONDRAW=0"])):
        add_U(o, oid, f"ondraw31 {oid}, Quality: {what}; Beach Bowl 120 s then Honeyhive 120 s (enemies, NPCs), scripted; GXV_BOWL_POSE stays 0", ["beach", "honey"], 120, 120, extra=QT + ex, quality=True, eyes="optional")
    # parprio31: the particle worker's priority and pause (Quality, the three modes at Honeyhive and Beach Bowl, A-B-C then A again)
    o.n = FIRST + 150
    for aid, what, ex in (("PP-A", "GXV_PAR_PRIO=0 GXV_PAR_PAUSE=0 (today)", ["GXV_PAR_PRIO=0", "GXV_PAR_PAUSE=0"]),
                          ("PP-B", "GXV_PAR_PRIO=1 GXV_PAR_PAUSE=1 (core-3 par workers one step above the finishing thread; proportional pause)", ["GXV_PAR_PRIO=1", "GXV_PAR_PAUSE=1"]),
                          ("PP-C", "GXV_PAR_PRIO=1 GXV_PAR_PAUSE=2 (no pause; only if PP-B still stalls)", ["GXV_PAR_PRIO=1", "GXV_PAR_PAUSE=2"]),
                          ("PP-Ab", "GXV_PAR_PRIO=0 GXV_PAR_PAUSE=0 (second control)", ["GXV_PAR_PRIO=0", "GXV_PAR_PAUSE=0"])):
        add_U(o, aid, f"parprio31 {aid}, Quality: {what}; Honeyhive 150 s then Beach Bowl 150 s; read par-prio: / par-ptcl: (joins max, stalls, paused turns), threads: core 3, smooth:", ["honey", "beach"], 150, 150,
              extra=["GXV_TURN_PROF=1"] + ex, quality=True)
    # texup31: the swizzled upload's pool and lazy fence (stage loads: no route)
    o.n = FIRST + 155
    for tid, what, ex in (("TU-0", "GXV_TEXUP_CENSUS=1 alone (the split and the 'soft census' field, the old 691 ms question)", ["GXV_TEXUP_CENSUS=1"]),
                          ("TU-1", "GXV_TEXUP_CENSUS=1 GXV_TEXUP_LAZYFENCE=1 GXV_TEXUP_SCRATCH=1 (transfers left pending, scratch pooled; same bytes: wait and alloc per upload, ms an upload per stage)", ["GXV_TEXUP_CENSUS=1", "GXV_TEXUP_LAZYFENCE=1", "GXV_TEXUP_SCRATCH=1"]),
                          ("TU-V", "TU-1 + GXV_TEXUP_VERIFY=1 (the swizzled chain compared with the linear one: 'differing 0' is the exactness proof; judge no timing)", ["GXV_TEXUP_CENSUS=1", "GXV_TEXUP_LAZYFENCE=1", "GXV_TEXUP_SCRATCH=1", "GXV_TEXUP_VERIFY=1"])):
        add_U(o, tid, f"texup31 {tid}: {what}; Observatory then Honeyhive, stage loads, no route", ["obs", "honey"], 40, 60, extra=ex, route=False)
    # asyncsh31: the shader compile worker, the stand-in level and the ubershader census (a fresh tagged cache each run: the owner's cache is never written)
    o.n = FIRST + 160
    AS = ["GXV_PACK_MISSLOG=1", "GXV_HITCH25_LOG=1", "GXV_UBERSHADER=1", "GXV_LOG_LEVEL=2"]
    add_U(o, "AS-A", "asyncsh31 A, baseline + census (tag c31a, fresh cache): worker off; read 'first-use: ubershader census:', 'pack: a shader compile ran in play', hitch: / hitch25: / first-use: lines; Observatory then Honeyhive",
          ["obs", "honey"], 60, 120, tag="c31a", extra=AS)
    for k, (tg, ex, what) in enumerate((("c31b1", [], "GXV_ASYNC_SHADERS=1, run 1"), ("c31b2", [], "GXV_ASYNC_SHADERS=1, run 2 (a new tag)"), ("c31b3", [], "GXV_ASYNC_SHADERS=1, run 3 (a new tag)"),
                                       ("c31m2", ["GXV_MISS_STANDIN=2"], "GXV_ASYNC_SHADERS=1 and GXV_MISS_STANDIN=2 (opaque map pieces stand in too)")), 1):
        add_U(o, f"AS-B{k}" if k < 4 else "AS-M2", f"asyncsh31 B, worker on (tag {tg}, fresh cache): {what}; pass: no crash, threads: smg_shaders mask 0x60000, async-shaders-5s refused 0 / pooled reallocs 0 / longest wait well under 15 s, no skippable compile on the render thread in play; report the hitch33-standin: counts",
              ["obs", "honey"], 60, 120, tag=tg, extra=AS + ["GXV_ASYNC_SHADERS=1"] + ex)
    # gxdedup31 recommended GXV_ARRAYSKIP=1 alone as the cheaper first A/B (lighter25; exact; verify = 2); Quality, the CPU-arm route
    o.n = FIRST + 170
    for kid, what, ex in (("AK-V", "GXV_ARRAYSKIP=2 (verify: nothing left out, the writes 1 would leave out go behind markers; the front end's 'arrayskip verify:' line must say 0 differing)", ["GXV_ARRAYSKIP=2"]),
                          ("AK-0", "GXV_ARRAYSKIP=0 (control)", ["GXV_ARRAYSKIP=0"]),
                          ("AK-1", "GXV_ARRAYSKIP=1 (a J3D array base the stream already holds is not written again; read the 'arrayskip:' line, writes and left out a picture)", ["GXV_ARRAYSKIP=1"]),
                          ("AK-0b", "GXV_ARRAYSKIP=0 (second control, off-on-off)", ["GXV_ARRAYSKIP=0"])):
        add_U(o, kid, f"arrayskip {kid}, Quality: {what}; Honeyhive 120 s then Beach Bowl 120 s, scripted, turn profile on", ["honey", "beach"], 120, 120, extra=["GXV_TURN_PROF=1"] + ex, quality=True)
    # strk1118 / fable-review-integ31c S3: GXV_FCLOSE_LOCK=1 is on (a bug fix); can the game thread wait for another thread's card write? Hitch count with it on and off, a cold tagged cache so the card is written to
    o.n = FIRST + 175
    for fid, tg, v in (("FL-1", "c31f1", 1), ("FL-0", "c31f0", 0), ("FL-1b", "c31g1", 1), ("FL-0b", "c31g0", 0)):
        add_U(o, fid, f"fclose lock {'on (the default)' if v else 'OFF (GXV_FCLOSE_LOCK=0)'}, cold tagged cache {tg} so the card is written to: Observatory then Honeyhive; compare hitch: / hitches over 100 ms (off, on, off, on pairs)",
              ["obs", "honey"], 60, 120, tag=tg, extra=["GXV_HITCH25_LOG=1", f"GXV_FCLOSE_LOCK={v}"])
    # fur31: the third fur arm (the replacement rule by the coat's screen size); keepdepth and fur A/C are above
    o.n = FIRST + 180
    add_O(o, "FUR-B", "fur B: GXV_FUR25=1 (shells by the coat's size on the screen, the dropped shells' alpha merged; full within 700 units of the player) with GXV_POPIN_LOG=1 GXV_FURCENSUS=1; Honeyhive, same Queen climb as FUR-A: furcensus: shells a picture against FUR-A, a thinner or paler pelt?",
          ["honey"], 120, 240, extra=["GXV_POPIN_LOG=1", "GXV_FURCENSUS=1", "GXV_FUR25=1"])
    add_O(o, "V7", "pose phase V7: GXV_DIAG_FULL_FAR_POSE=1 (restores the far-logic level-3 half-rate pose only; log 'far-pose-test: restored'); Honeyhive bee suit 150 s, then the Observatory; compare with V0",
          ["honey", "obs"], 150, 240, extra=["GXV_POSE_PHASE_CENSUS=1", "GXV_DIAG_FULL_FAR_POSE=1"], bee=True)
    # census runs (log only, read the numbers; each alone, because the censuses' own timers nest)
    o.n = FIRST + 185
    add_U(o, "CENS-FP", "census to read: GXV_FEPLAN_CENSUS=1 alone (feplan31): 'a plan could take N (S state cmds, D draws, T ms)', by way, by barrier, the eight costliest lists; go on only if T is 0.3 ms or more with S/N about 20 or more; Observatory, Honeyhive, Beach Bowl",
          ["obs", "honey", "beach"], 60, 90, extra=["GXV_FEPLAN_CENSUS=1"], route=False)
    add_U(o, "CENS-TL", "census to read: GXV_TINYLIST_CENSUS=1 alone (tinylist31): 'tiny calls per picture', 'state only N', 'could replay N (ms)' (go on only if 0.5 ms or more at Honeyhive); Observatory, Honeyhive, Beach Bowl",
          ["obs", "honey", "beach"], 60, 90, extra=["GXV_TINYLIST_CENSUS=1"], route=False)
    add_U(o, "CENS-PJ", "census to read: palette-join census (paljoin31, plan-gpu31 A.7): GXV_PALJOIN_CENSUS=1 GXV_FE_CENSUS=1 GXV_STREAM_CENSUS=60 GXV_BE_PROF=4 GXV_PAR_ANIM_WHY=1; Honeyhive then Beach Bowl; judge no ms (the censuses cost time)",
          ["honey", "beach"], 90, 90, extra=["GXV_PALJOIN_CENSUS=1", "GXV_FE_CENSUS=1", "GXV_STREAM_CENSUS=60", "GXV_BE_PROF=4", "GXV_PAR_ANIM_WHY=1"], route=False)
    add_U(o, "CENS-GPU", "census to read: GXV_GPU_SCENES=2 GXV_GPU_CENSUS=1 (gpucensus31: textures per fragment by class, pixels a picture, parameter-buffer estimate), Quality, Dreadnought far zoom then Beach Bowl, no route; read GPU figures only",
          ["dread", "beach"], 120, 120, extra=["GXV_GPU_SCENES=2", "GXV_GPU_CENSUS=1"], quality=True, route=False)
    # vglup31: the vitaGL test archives. NOT env switches: each is a build (out/vglA, out/vglB, both from console4 e76e06fc, whose eboot is the control). The relay installs the eboot first (as for console2/4), then the SAME job runs.
    o.n = FIRST + 190
    for vid, what in (("VGL-0", "control: the console4 eboot (e76e06fc, kit vitaGL archive)"), ("VGL-A", "install out/vglA first (aed669ae...: vitaGL micro-opts, glFinish with no empty scene)"),
                      ("VGL-B", "install out/vglB first (563dc274...: A + NO_TILE_CLIPPER: less CPU, more GPU)"), ("VGL-0b", "control again: reinstall the console4 eboot (drift)")):
        add_U(o, vid, f"vitaGL archive A/B {vid}, Performance: {what}; Honeyhive 120 s then Beach Bowl 120 s, scripted; read shown fps, frame/pair p95, turn profile, GPU ms (B moves work to the GPU); no env switch", ["honey", "beach"], 120, 120,
              extra=["GXV_TURN_PROF=1"])
    # mariolag31 (docs/console31/mariolag31-arms.md): Mario one picture behind in 60 Assist. GXV_HRW_MARIOTIME 0 (census) / 1 / 2, control twice (the Beach Bowl states:
    # compare only visits whose draws a picture match), then two flip-book arms for the pictures round his landings. Performance + 60 Assist, the scripted route jumps (A).
    o.n = 8400
    MTC = ["GXV_HRW_MARIOTIME_CENSUS=1"]
    for mid, what, ex in (("MT-0", "the control with the census (GXV_HRW_MARIOTIME_CENSUS=1): 'STALE Mario shown' by reason = today's lag", MTC),
                          ("MT-1", "GXV_HRW_MARIOTIME=1 (no stale Mario: such a second picture is held as the real 30 picture; the old auto rule)", ["GXV_HRW_MARIOTIME=1"]),
                          ("MT-2", "GXV_HRW_MARIOTIME=2 (as 1, and the fresh player's auto rule by its own cost, the late-turn sleep left out)", ["GXV_HRW_MARIOTIME=2"]),
                          ("MT-0b", "the control again (state match)", MTC),
                          ("MT-2b", "GXV_HRW_MARIOTIME=2 again (state match)", ["GXV_HRW_MARIOTIME=2"])):
        add_U(o, mid, f"mariolag31 {mid}, Performance + 60 Assist: {what}; Honeyhive 120 s then Beach Bowl 90 s, scripted (jumps); read hrw: mariotime / fresh auto / fresh deadline / pictures, late-turn, fps",
              ["honey", "beach"], 120, 90, extra=ex, shots=3, eyes="optional")
    MTF = ["GXV_HRW_FLIPBOOK=240", "GXV_HRW_FLIPBOOK_FROM=600"]   # (half-size PPMs of every picture in ux0:data/smg/hrw31/; clear the folder between the two arms)
    for mid, what, ex in (("MT-F0", "flip-book, the control with the census", MTC), ("MT-F2", "flip-book, GXV_HRW_MARIOTIME=2", ["GXV_HRW_MARIOTIME=2"])):
        add_U(o, mid, f"mariolag31 {mid}, Performance + 60 Assist: {what}; Honeyhive 120 s, scripted (jumps); pull ux0:data/smg/hrw31/ after the run; B pictures at his landings",
              ["honey"], 120, 120, extra=MTF + ex, eyes="no")
    add_O(o, "MT-O2", "mariolag31 owner's eyes: GXV_HRW_MARIOTIME=2 with the census; Honeyhive then Beach Bowl, Performance + 60 Assist: jump and land again and again (short hops, long jumps, "
          "a ground pound), run and stop; does Mario hang in the air for a picture after a landing? Then MT-O0 the same way (today's code) for the comparison", ["honey", "beach"], 120, 240,
          extra=["GXV_HRW_MARIOTIME=2"])
    add_O(o, "MT-O0", "mariolag31 owner's eyes, the control: today's code with the census only (GXV_HRW_MARIOTIME_CENSUS=1); the same play as MT-O2", ["honey", "beach"], 120, 240, extra=MTC)
    # strk31: the console's own compiled shaders, copied back READ-ONLY (docs/CONSOLE-SESSION-3.1.md section 8). Strikers' relay does the same (pull_cache) and got 21 programs
    # its emulator walk had missed: a .gxp is written only by a real compile. close + ls + pull_tree + ls: no put, no delete, nothing is written on the console.
    o.n = 8985
    n = o.num(); o.save(f"{n}-c31-pull-shadercache", [{"action": "close"}, {"sleep": 4}, {"action": "ls", "path": "ux0:data/smg/shadercache"},
                        {"action": "pull_tree", "path": "ux0:data/smg/shadercache", "max_mb": 64}, {"action": "ls", "path": "ux0:data/smg"}], 60,
                        {"kind": "U", "what": "section 8: close the game and copy ux0:data/smg/shadercache/<key>/ (read-only; both lists, the .ply, the tagged -c31* folders) to outbox/<job>/ for refill_pack.py --pulled", "eyes": "no"})
    o.n = 8990
    n = o.num(); o.save(f"{n}-c31-restore", START[:2] + [{"action": "clear_input"}, {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}, {"action": "pull_logs"},
                        {"action": "ls", "path": "ux0:data/smg"}], 30, {"kind": "U", "what": "section 3: close the game, remove input.txt, put the owner's env.txt back", "eyes": "no"})

def write_index(o):
    out = []
    for name, secs, r in sorted(o.rows, key=lambda x: x[0]):
        out.append(f"{name:34s} {r['kind']} {secs / 60:5.1f} min  eyes:{r['eyes']:8s} {r['what']}")
    (o.out / "JOBS.txt").write_text("\n".join(out) + "\n")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(REPO / "docs" / "console31"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("all"); sub.add_parser("list")
    a1 = sub.add_parser("install"); a1.add_argument("--relay", required=True)
    a2 = sub.add_parser("queue"); a2.add_argument("--relay", required=True); a2.add_argument("names", nargs="+")
    a = ap.parse_args(); out = Path(a.out).resolve()
    if a.cmd == "all":
        o = Out(out); build(o); write_index(o); print(f"{len(o.rows)} jobs in {o.jobs}"); return
    if a.cmd == "list": print((out / "JOBS.txt").read_text(), end=""); return
    relay = Path(a.relay).resolve()
    for d in ("staging", "inbox", "done"):
        if not (relay / d).is_dir(): raise SystemExit(f"{relay} is not the relay folder (no {d}/)")
    jd = relay / "claude-jobs-console31"
    if a.cmd == "install":
        jd.mkdir(exist_ok=True)
        for p in (out / "staging").iterdir():
            if p.name == "env-user.txt": print("skipped staging/env-user.txt (the owner's own env.txt is never installed by this tool: make it from the backup, section 0)"); continue
            shutil.copyfile(p, relay / "staging" / p.name)
        for p in (out / "jobs").glob("*.json"): shutil.copyfile(p, jd / p.name)
        print("installed", len(list((out / "jobs").glob("*.json"))), "jobs into", jd); return
    for n in a.names:
        src = jd / f"{n}.json"
        if not src.exists(): raise SystemExit(f"no job {src}")
        if (relay / "inbox" / src.name).exists() or (relay / "done" / src.name).exists(): raise SystemExit(f"{n} is in the inbox or has run (relay/done): make a new name")
        shutil.copyfile(src, relay / "inbox" / f".{src.name}.part"); os.replace(relay / "inbox" / f".{src.name}.part", relay / "inbox" / src.name); print("queued", n)

if __name__ == "__main__": main()

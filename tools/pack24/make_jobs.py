#!/usr/bin/env python3
"""make_jobs.py: the relay jobs of the shader-pack refill (pack24, Galaxy Vita 2.3). Pure Python 3.8+, no network, no console: it only writes files.

  python3 make_jobs.py all   [--out jobs] [--quick] [--pack-lists DIR] [--route DIR]
        writes <out>/staging/ (env-p24-*.txt, input-p24-*.txt, p24-lists/) and <out>/NNN-p24-*.json, <out>/JOBS.txt
  python3 make_jobs.py stops <name> "Galaxy:1,Galaxy:2,..." [--perf] [--hold 480] [--flow]
        one more tour job (to finish a leg a crash cut short: give the stops after the last "flow: entry K" / "tour: jumping" line of its log)
  python3 make_jobs.py nav <name> "press up; wait 800ms; release up" [...]    a few button presses on the running game and a shot after each
  python3 make_jobs.py restore <name> <ux0:data/smg/shadercache.aside-<stamp>-pre24>   put the owner's cache back (the stamp: job 700's ls)
  python3 make_jobs.py install --relay <.../relay> [--out jobs]    copies <out>/staging/* (not env-user.txt) to relay/staging and the jobs to relay/claude-jobs-pack24
  python3 make_jobs.py queue   --relay <relay> <name>...            copies claude-jobs-pack24/<name>.json into relay/inbox (refuses one that is there or ran)

The refill runs with GXV_SHADERPACK=0 and GXV_VITA_SEED=off (see README): the 2.2 package seed answers nothing, so every text this build needs is compiled
onto the card; the owner's cache is set aside first (aside job) and the owner's env-user.txt is put back at the end of every job. The env files carry only
the listed switches (none changes a shader text): the owner's own overrides never leak in, and the refill's do not stay.
"""
import argparse, json, os, shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIRST = 700

def vc(c, w=1.0): return {"action": "vc", "cmd": c, "wait": w}
X = "press cross; wait 150ms; release cross"
TO_FILE2 = [{"action": "launch"}, {"sleep": 40}, vc("press start; wait 150ms; release start"), {"sleep": 8},
            vc("press right; wait 250ms; release right; press down; wait 400ms; release down", 2), {"sleep": 1}, vc(X), {"sleep": 3},
            vc("press right; wait 700ms; release right; press down; wait 220ms; release down; press left; wait 150ms; release left", 2),
            {"sleep": 0.5}, vc(X), {"sleep": 15}]
BOOT_S = 72
START = [{"action": "close"}, {"sleep": 3}, vc("screen on", 2), vc("unlock", 3), vc("nosleep on"), {"action": "clear_input"}]
END = [vc("release all", 1), {"action": "close"}, {"sleep": 4}, {"action": "pull_logs"}, {"action": "clear_input"},
       {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}, vc("screen off", 1)]

REFILL = """# pack24 shader-pack refill. Only switches that leave every shader text as the game's defaults make it:
# GXV_SHADERPACK=0 and GXV_VITA_SEED=off: neither the package's RAM pack nor its loose seed answers anything, so every text is compiled onto the card.
GXV_SHADERPACK=0
GXV_VITA_SEED=off
"""
PERF = "# Performance mode (60 target), whatever player-settings.cfg says\nGXV_FPS_LIMIT=0\nGXV_AUTOQ_TARGET=60\n"
QUAL = "# Quality mode (30), whatever player-settings.cfg says\nGXV_FPS_LIMIT=30\nGXV_AUTOQ_TARGET=30\n"
SCRIPT = "PETARI_SCRIPT_ANCHOR=play\nPETARI_SCRIPT_LOG=1\n"

def L(name): return [l.strip() for l in (HERE / name).read_text().split() if l.strip()]
ALL = [s for l in L("tour_legs.txt") for s in l.split(",")]
S0_G = ["AstroDome", "AstroGalaxy", "LibraryRoom", "PeachCastleFinalGalaxy", "PeachCastleGardenGalaxy"]   # no star select of their own: PETARI_STAGE
S0 = [s for s in ALL if s.split(":")[0] in S0_G]
FLOW_G = list(dict.fromkeys(s.split(":")[0] for s in ALL if s.split(":")[0] not in S0_G))      # 41 galaxies, flow entry :1
REST = [s for s in ALL if s.split(":")[0] not in S0_G and not s.endswith(":1")]                # 79 more scenarios, PETARI_STAGE
# richest galaxies by records in the 2.2 stagewarm.bin (Performance flow, 24 entries = PETARI_FLOW's limit)
PERF_G = ["HeavenlyBeachGalaxy", "OceanRingGalaxy", "EggStarGalaxy", "HoneyBeeKingdomGalaxy", "IceVolcanoGalaxy", "OceanPhantomCaveGalaxy", "KoopaBattleVs1Galaxy",
          "TransformationExGalaxy", "CannonFleetGalaxy", "PhantomGalaxy", "KoopaBattleVs3Galaxy", "SandClockGalaxy", "KoopaBattleVs2Galaxy", "FactoryGalaxy",
          "StarDustGalaxy", "KoopaJrShipLv1Galaxy", "TearDropGalaxy", "FloaterOtaKingGalaxy", "CosmosGardenGalaxy", "FishTunnelGalaxy", "BattleShipGalaxy",
          "DarkRoomGalaxy", "CubeBubbleExLv2Galaxy", "SurfingLv2Galaxy"]
PU_STAGES = ["HoneyBeeKingdomGalaxy:1", "IceVolcanoGalaxy:1", "FactoryGalaxy:1", "OceanPhantomCaveGalaxy:1", "PhantomGalaxy:1", "HeavensDoorGalaxy:2"]
PU_LIST = "4,0,3,0,5,0,1,0,6,0,2,0,7,0"   # bee, cure, ice, cure, spring, cure, rainbow, cure, boo, cure, fire, cure, flying, cure
PU_EVERY, PU_AT = 300, 240
PU_HOLD = PU_AT + 14 * PU_EVERY + 60

def chunks(xs, n):
    k = -(-len(xs) // n); return [xs[i:i + k] for i in range(0, len(xs), k)]

class Out:
    def __init__(self, out): self.out = out; self.stg = out / "staging"; self.stg.mkdir(parents=True, exist_ok=True); self.listing = []
    def env(self, name, text): (self.stg / f"env-{name}.txt").write_text(text); return f"env-{name}.txt"
    def save(self, name, steps, what, secs):
        (self.out / f"{name}.json").write_text(json.dumps({"action": "batch", "steps": steps}))
        self.listing.append(f"{name:24s} {secs / 60:5.1f} min  {what}")

def run_steps(env, inp, secs, shots=True):
    """launch to file 2, then idle through the scripted input; a shot every ~90 s"""
    steps = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}]
    if inp: steps.append({"action": "put", "src": inp, "path": "ux0:data/smg/input.txt"})
    steps += TO_FILE2
    t, k = 0.0, 0
    secs -= BOOT_S
    while secs - t > 0.5:
        d = round(min(90.0, secs - t), 1); steps.append({"sleep": d}); t += d
        if shots and secs - t > 5: k += 1; steps.append({"action": "shot", "name": f"t{k:02d}"})
    return steps + END

def flow_secs(n, hold, slack=8): return BOOT_S + n * (12 + 5 + 3 + slack + hold / 60 * 1.3) + 30
def stage_secs(n, hold, slack=8): return BOOT_S + n * (14 + 3 + slack + hold / 60 * 1.3) + 30

def tour_job(o, name, stops, hold, perf, flow, route="input-p24-route.txt", extra="", what=""):
    if flow and len(stops) > 24: raise SystemExit(f"{name}: PETARI_FLOW takes at most 24 entries ({len(stops)})")
    line = ",".join(stops)
    if len(line) > 2000: raise SystemExit(f"{name}: tour line of {len(line)} chars (entry.cpp reads 2 KiB lines)")
    key = "PETARI_FLOW" if flow else "PETARI_STAGE"
    body = f"{key}={line}\n" + (f"PETARI_FLOW_HOLD={hold}\nPETARI_FLOW_LOG=1\n" if flow else f"PETARI_STAGE_HOLD={hold}\n")
    env = o.env(name, REFILL + (PERF if perf else QUAL) + SCRIPT + extra + f"# {name}: {len(stops)} entries, {hold} turns of play each\n" + body)
    secs = (flow_secs if flow else stage_secs)(len(stops), hold)
    o.save(name, run_steps(env, route, secs), what or f"{'flow' if flow else 'stage'} tour of {len(stops)}: {stops[0]} .. {stops[-1]} ({'Performance' if perf else 'Quality'})", secs)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="jobs"); sub = ap.add_subparsers(dest="cmd", required=True)
    a1 = sub.add_parser("all"); a1.add_argument("--quick", action="store_true"); a1.add_argument("--warm-min", type=float, default=20)
    a1.add_argument("--pack-lists", help="a folder holding the package's vgx_shaderlist.bin (copied to staging/p24-lists)"); a1.add_argument("--route", help="folder with route-p24.txt, route-pu-p24.txt")
    a2 = sub.add_parser("stops"); a2.add_argument("name"); a2.add_argument("stops"); a2.add_argument("--perf", action="store_true")
    a2.add_argument("--hold", type=int, default=480); a2.add_argument("--flow", action="store_true")
    a3 = sub.add_parser("nav"); a3.add_argument("name"); a3.add_argument("cmds", nargs="+")
    a6 = sub.add_parser("restore"); a6.add_argument("name"); a6.add_argument("aside", help="the aside entry job 700's ls showed, e.g. ux0:data/smg/shadercache.aside-20261110-1200-pre24")
    a4 = sub.add_parser("install"); a4.add_argument("--relay", required=True)
    a5 = sub.add_parser("queue"); a5.add_argument("--relay", required=True); a5.add_argument("names", nargs="+")
    a = ap.parse_args()
    out = Path(a.out).resolve()
    if a.cmd in ("install", "queue"):
        relay = Path(a.relay).resolve()
        for d in ("staging", "inbox", "done"):
            if not (relay / d).is_dir(): raise SystemExit(f"{relay} is not the relay folder (no {d}/)")
        jd = relay / "claude-jobs-pack24"
        if a.cmd == "install":
            jd.mkdir(exist_ok=True)
            for p in (out / "staging").iterdir():
                if p.name == "env-user.txt": continue
                if p.is_dir(): shutil.copytree(p, relay / "staging" / p.name, dirs_exist_ok=True)
                else: shutil.copyfile(p, relay / "staging" / p.name)
            for p in out.glob("*.json"): shutil.copyfile(p, jd / p.name)
            print("installed", len(list(out.glob('*.json'))), "jobs into", jd); return
        for n in a.names:
            src = jd / f"{n}.json"
            if not src.exists(): raise SystemExit(f"no job {src}")
            if (relay / "inbox" / src.name).exists() or (relay / "done" / src.name).exists(): raise SystemExit(f"{n} is in the inbox or has run (relay/done): make a new name")
            shutil.copyfile(src, relay / "inbox" / f".{src.name}.part"); os.replace(relay / "inbox" / f".{src.name}.part", relay / "inbox" / src.name); print("queued", n)
        return
    o = Out(out)
    if a.cmd == "stops":
        tour_job(o, a.name, [s for s in a.stops.split(",") if s], a.hold, a.perf, a.flow)
    elif a.cmd == "restore":
        o.save(a.name, [{"action": "close"}, {"sleep": 3}, {"action": "restore_aside", "path": a.aside}, {"action": "ls", "path": "ux0:data/smg"},
                        {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}], "restore the owner's shader cache (whatever has the name now is set aside first)", 30)
    elif a.cmd == "nav":
        steps = []
        for i, c in enumerate(a.cmds): steps += [vc(c, 2), {"sleep": 2}, {"action": "shot", "name": f"n{i:02d}"}]
        o.save(a.name, steps, "button presses + shots on the running game", 6 * len(a.cmds))
    else:
        n = FIRST
        def nm(tag):
            nonlocal n; s = f"{n}-p24-{tag}"; n += 1; return s
        if a.route:
            for f in ("route-p24.txt", "route-pu-p24.txt"): shutil.copyfile(Path(a.route) / f, o.stg / f"input-{f[6:]}")
        if a.pack_lists:
            d = o.stg / "p24-lists"; d.mkdir(exist_ok=True); shutil.copyfile(Path(a.pack_lists) / "vgx_shaderlist.bin", d / "vgx_shaderlist.bin")
        R, PU = "input-p24.txt", "input-pu-p24.txt"
        # 0. aside + eboot, and the package's warm-up list onto the (now empty) card cache
        steps = [{"action": "close"}, {"sleep": 3}, {"action": "put", "src": "eboot-CANDIDATE.bin", "path": "ux0:app/SMGV00001/eboot.bin"},
                 {"action": "ls", "path": "ux0:data/smg/shadercache"}, {"action": "aside", "path": "ux0:data/smg/shadercache", "tag": "pre24"},
                 {"action": "ls", "path": "ux0:data/smg"},
                 {"action": "put_tree", "src": "p24-lists", "dest": "ux0:data/smg/shadercache", "replace": False, "max_mb": 8}, {"action": "ls", "path": "ux0:data/smg/shadercache"}]
        o.save(nm("aside"), steps, "eboot-CANDIDATE.bin installed; card cache -> shadercache.aside-<stamp>-pre24; package vgx_shaderlist.bin put on the card", 60)
        # 1. warm: idle at the title; compiles the whole warm-up list (about 2,429 texts)
        env = o.env("p24-warm", REFILL + QUAL)
        secs = a.warm_min * 60 + 90
        steps = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}, {"action": "launch"}, {"sleep": 45}, {"action": "shot", "name": "title"}]
        for i in range(int(a.warm_min * 60 // 120)): steps += [{"sleep": 115}, {"action": "shot", "name": f"w{i:02d}"}]
        o.save(nm("warm"), steps + END, f"idle at the title {a.warm_min:g} min (log must say 'deferred shader warm-up finished'; if not, run warm2)", secs)
        env = o.env("p24-warm2", REFILL + QUAL)
        steps = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}, {"action": "launch"}, {"sleep": 45}]
        for i in range(5): steps += [{"sleep": 115}, {"action": "shot", "name": f"w{i:02d}"}]
        o.save(nm("warm2"), steps + END, "OPTIONAL second warm-up, 11 min (only if the first log lacks 'deferred shader warm-up finished')", 11 * 60 + 60)
        # 2. Quality flow, every galaxy at scenario 1 (star select, menu list, file select, player programs), first-use route + pause in each
        for i, g in enumerate(chunks(FLOW_G, 2)):
            tour_job(o, nm(f"A{i + 1}"), [f"{x}:1" for x in g], 1560, False, True, R, what=f"Quality flow {len(g)} galaxies at :1 with the first-use route: {g[0]} .. {g[-1]}")
        # 3. the stages without a star select
        tour_job(o, nm("S0"), S0, 600, False, False, R, what=f"PETARI_STAGE {len(S0)} stops: Observatory domes + rooms, Peach castle")
        # 4. player forms
        env = o.env("p24-pu", REFILL + QUAL + SCRIPT + f"PETARI_FLOW={','.join(PU_STAGES)}\nPETARI_FLOW_HOLD={PU_HOLD}\nPETARI_FLOW_LOG=1\n"
                    f"PETARI_POWERUP={PU_LIST}\nPETARI_POWERUP_AT={PU_AT}\nPETARI_POWERUP_EVERY={PU_EVERY}\n")
        secs = flow_secs(len(PU_STAGES), PU_HOLD)
        o.save(nm("PU"), run_steps(env, PU, secs), "player forms: bee, ice, spring, rainbow, boo, fire, flying (flow of 6 stages, a form cycle in each)", secs)
        # 5. the Observatory flight (domes)
        env = o.env("p24-dome", REFILL + QUAL + SCRIPT + "PETARI_FLOW=EggStarGalaxy:1,HoneyBeeKingdomGalaxy:1,BattleShipGalaxy:1\nPETARI_FLOW_DOME=1\nPETARI_FLOW_HOLD=300\nPETARI_FLOW_LOG=1\n")
        secs = BOOT_S + 3 * 75 + 30
        o.save(nm("DOME"), run_steps(env, None, secs), "flow with PETARI_FLOW_DOME=1: dome, galaxy-map flight and white flash before each of 3 galaxies", secs)
        # 6. picture-size rungs: the controller's ladder in two stages (CharaAA, copy variants, upscale fallbacks), then the MSAA rungs
        env = o.env("p24-rung", REFILL + PERF + SCRIPT + "GXV_AUTOQ_CYCLE=6\nPETARI_STAGE=EggStarGalaxy:1,HeavenlyBeachGalaxy:1\nPETARI_STAGE_HOLD=4500\n")
        secs = stage_secs(2, 4500)
        o.save(nm("RUNG"), run_steps(env, R, secs), "GXV_AUTOQ_CYCLE=6 (rungs 0-6 and back, twice) in two stages with Mario in view", secs)
        env = o.env("p24-rung8", REFILL + QUAL + SCRIPT + "GXV_AUTOQ_LEVEL=8\nPETARI_STAGE=EggStarGalaxy:1,HeavenlyBeachGalaxy:1\nPETARI_STAGE_HOLD=1560\n")
        secs = stage_secs(2, 1560)
        o.save(nm("RUNG8"), run_steps(env, R, secs), "GXV_AUTOQ_LEVEL=8 (960x544, 4x multisampling), two stages; level 7 is the Quality start rung in the A jobs", secs)
        # 7. pull
        pull = [{"action": "close"}, {"sleep": 4}, {"action": "pull_logs"}, {"action": "ls", "path": "ux0:data/smg/shadercache"},
                {"action": "pull_tree", "path": "ux0:data/smg/shadercache", "max_mb": 64},
                {"action": "put", "src": "env-user.txt", "path": "ux0:data/smg/env.txt"}, vc("screen off", 1)]
        o.save(nm("pull"), pull, "pull_tree ux0:data/smg/shadercache (key folder, both lists, the .ply) -> outbox/<job>/..; QUICK SET ENDS HERE", 300)
        if not a.quick:
            tour_job(o, nm("P"), [f"{g}:1" for g in PERF_G], 1000, True, True, R, what="OPTIONAL Performance flow of the 24 richest galaxies")
            for i, st in enumerate(chunks(REST, 4)):
                tour_job(o, nm(f"B{i + 1}"), st, 480, False, False, R, what=f"OPTIONAL PETARI_STAGE sweep of scenarios 2+: {st[0]} .. {st[-1]} ({len(st)} stops)")
            o.save(nm("pull2"), pull, "pull again after the optional jobs (the first pull is superseded)", 300)
        # 8. verification (after refill_pack.py made <out>/shadercache; stage it as staging/p24-seed)
        o.save(nm("seedput"), [{"action": "close"}, {"sleep": 3},
               {"action": "put_tree", "src": "p24-seed", "dest": "ux0:app/SMGV00001/shadercache", "replace": True, "max_mb": 64},
               {"action": "ls", "path": "ux0:app/SMGV00001/shadercache"}, {"action": "aside", "path": "ux0:data/smg/shadercache", "tag": "post24"}, {"action": "ls", "path": "ux0:data/smg"}],
               "put the rebuilt seed into the installed app (the old files set aside there); the refill's card cache set aside again", 600)
        DEF = "# pack24 verification: the game's defaults (RAM pack on, package seed), nothing else\n"
        env = o.env("p24-v1", DEF + QUAL)
        secs = 60 * 4 + 90
        steps = START + [{"action": "put", "src": env, "path": "ux0:data/smg/env.txt"}, {"action": "launch"}, {"sleep": 45}, {"action": "shot", "name": "title"}]
        for i in range(int(180 // 90)): steps += [{"sleep": 90}, {"action": "shot", "name": f"v{i}"}]
        o.save(nm("V1"), steps + END, "verify 1: fresh start at the title, 4 min: warm-up links 0, 'neither' 0", secs)
        vg = [f"{g}:1" for g in PERF_G[:12]]
        env = o.env("p24-v2", DEF + QUAL + SCRIPT + f"PETARI_FLOW={','.join(vg)}\nPETARI_FLOW_HOLD=1560\nPETARI_FLOW_LOG=1\n")
        secs = flow_secs(len(vg), 1560, 0)
        o.save(nm("V2"), run_steps(env, R, secs), f"verify 2: Quality flow of the 12 richest galaxies with the route: stage warm-up links 0, 0 'in play' programs", secs)
        env = o.env("p24-v3", DEF + PERF + SCRIPT + "GXV_AUTOQ_CYCLE=6\nPETARI_STAGE=EggStarGalaxy:1,HeavenlyBeachGalaxy:1\nPETARI_STAGE_HOLD=4500\n")
        secs = stage_secs(2, 4500, 0)
        o.save(nm("V3"), run_steps(env, R, secs), "verify 3: Performance + rung cycle in two stages: no program linked at a rung change", secs)
        env = o.env("p24-v4", DEF + QUAL + SCRIPT + f"PETARI_FLOW={','.join(PU_STAGES)}\nPETARI_FLOW_HOLD={PU_HOLD}\nPETARI_FLOW_LOG=1\n"
                    f"PETARI_POWERUP={PU_LIST}\nPETARI_POWERUP_AT={PU_AT}\nPETARI_POWERUP_EVERY={PU_EVERY}\n")
        secs = flow_secs(len(PU_STAGES), PU_HOLD, 0)
        steps = run_steps(env, PU, secs)
        i = max(j for j, s in enumerate(steps) if s.get("action") == "pull_logs") + 1
        steps[i:i] = [{"action": "pull_tree", "path": "ux0:data/smg/shadercache", "max_mb": 64}]
        o.save(nm("V4"), steps, "verify 4: the player forms again, then pull_tree of the card cache (must hold no new .gxp)", secs + 200)
    (out / "JOBS.txt").write_text("\n".join(o.listing) + "\n"); print("\n".join(o.listing))

if __name__ == "__main__": main()

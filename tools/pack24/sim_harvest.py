#!/usr/bin/env python3
"""sim_harvest.py (fix31): the merge of a whole emulator harvest, simulated on the host BEFORE the real one runs. Run it with python3 -I.

  python3 -I sim_harvest.py --package <.vpk | shadercache folder> --source <tree> --out <new folder> [--plan-args "..."] [--candraw-pool N]
        [--pool-frac F] [--take F] [--jnl-frac F] [--keep]

1. tools/seedwalk/plan.py make: the real leg split (default: 134 stops in two passes -> 71 legs).
2. tools/pack24/sim_pull.py --base-stage: one simulated card per leg, as the game leaves it: the package's own records (the leg starts from the candidate
   seed), then what the leg learned at each of its stops, labelled as the game labels a new record (the visit's scenario hint; one unhinted copy for a draw
   the same card saw in another scenario; a slow copy for some), can-draw records with --candraw-pool, the last share in the journal only. Real key pairs of
   the package's key list. Lists only: no compiled file.
3. tools/pack24/refill_pack.py with every leg as --harvest (lists-only; closure only reported: the package's own list may be another generator's).
Prints the merge line, every CAP failure (there must be none), the game-side caps the legs' cards reached (each would be a "cap-full:" line in that leg's
log, which fails misskeys.py check) and the result. Exit 0 when no cap is hit, 3 otherwise. Test data only: --out must be outside the repository.
harvprep31: the legs run GXV_SEEDWALK=1 (8,192 own records a stage on a leg's card; --old-cap: a build before, 2,048) and leave their draw evidence; the merge prunes
a stage over 2,048 by it (--prune fill|all|off, passed to refill_pack.py). --pkg-drawable F / --draw-take F: see sim_pull.py. Prints the prune lines.
"""
import argparse, json, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True); ap.add_argument("--source", default=str(ROOT)); ap.add_argument("--out", required=True)
    ap.add_argument("--plan-args", default="", help="extra options for plan.py make (default: its defaults, 71 legs)")
    ap.add_argument("--candraw-pool", type=int, default=0); ap.add_argument("--pool-frac", type=float, default=0.5); ap.add_argument("--take", type=float, default=0.7)
    ap.add_argument("--jnl-frac", type=float, default=0.1); ap.add_argument("--keep", action="store_true", help="keep the leg folders and the refill output")
    ap.add_argument("--old-cap", action="store_true"); ap.add_argument("--no-evidence", action="store_true"); ap.add_argument("--prune", default="fill")
    ap.add_argument("--pkg-drawable", type=float, default=0.75); ap.add_argument("--draw-take", type=float, default=0.7)
    a = ap.parse_args()
    out = Path(a.out).resolve()
    if out.exists(): raise SystemExit(f"sim_harvest: {out} exists")
    if str(out).startswith(str(ROOT) + os.sep): raise SystemExit("sim_harvest: --out is inside the repository")
    out.mkdir(parents=True)
    py = [sys.executable, "-I"]
    # the package's lists
    pk = Path(a.package)
    if pk.is_file():
        with zipfile.ZipFile(pk) as z:
            for n in ("shadercache/vgx_stagewarm.bin", "shadercache/vgx_shaderlist.bin"):
                (out / "pkg" / n).parent.mkdir(parents=True, exist_ok=True); (out / "pkg" / n).write_bytes(z.read(n))
        pkg_sc = out / "pkg" / "shadercache"
        refill_pkg = pk
    else:
        pkg_sc = pk / "shadercache" if (pk / "shadercache").is_dir() else pk
        refill_pkg = pkg_sc
    # tools
    tools = out / "tools"; tools.mkdir()
    src = Path(a.source)
    for cmd in (["g++", "-std=c++20", "-O1", f"-I{src}/platform/vgx/src", f"-I{src}/platform/vgx/third_party", str(src / "tools/pack24/listnames.cpp"),
                 str(src / "platform/vgx/src/vgx_shadergen.cpp"), "-o", str(tools / "listnames")],
                ["g++", "-std=c++20", "-O1", f"-I{src}/platform/vgx/src", str(src / "tests/vgx_profile_word.cpp"), "-o", str(tools / "pw")]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: raise SystemExit("sim_harvest: building a tool failed:\n" + r.stderr[-2000:])
    import importlib.util
    spec = importlib.util.spec_from_file_location("rp", HERE / "refill_pack.py"); rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)
    _, values, _, _ = rp.release_profile(src)
    penv = [x for k, v in values.items() for x in ("--profile-env", f"{k}={v}")]
    # the plan
    r = subprocess.run(py + [str(ROOT / "tools/seedwalk/plan.py"), "make", "--out", str(out / "plan")] + a.plan_args.split(), capture_output=True, text=True)
    if r.returncode: raise SystemExit("sim_harvest: plan.py make failed:\n" + r.stdout + r.stderr)
    plan = json.loads((out / "plan" / "plan.json").read_text())
    legs = plan["legs"]
    harvests, capfull = [], []
    for lid in legs:
        sp = json.loads((out / "plan" / "legs" / lid / "spec.json").read_text())
        stops = list(sp["stops"]) if sp["kind"] in ("flow", "stage", "mpdemo") else []
        if sp["kind"] in ("menus", "mpmenus"): stops = ["MENU:0"]
        h = out / "legs" / lid
        r = subprocess.run(py + [str(HERE / "sim_pull.py"), "--list", str(pkg_sc / "vgx_shaderlist.bin"), "--source", str(src), "--out", str(h), "--listnames",
                                 str(tools / "listnames"), "--no-files", "--base-stage", str(pkg_sc / "vgx_stagewarm.bin"), "--stops", ",".join(stops),
                                 "--candraw-pool", str(a.candraw_pool), "--pool-frac", str(a.pool_frac), "--take", str(a.take), "--jnl-frac", str(a.jnl_frac),
                                 "--profile-tool", str(tools / "pw"), "--pkg-drawable", str(a.pkg_drawable), "--draw-take", str(a.draw_take)]
                                 + (["--old-cap"] if a.old_cap else []) + (["--no-evidence"] if a.no_evidence else []) + penv, capture_output=True, text=True)
        if r.returncode: raise SystemExit(f"sim_harvest: sim_pull for {lid} failed:\n" + r.stdout + r.stderr)
        line = r.stdout.strip().splitlines()[-1]
        m = re.search(r"game cap-full drops: own (\d+), can-draw (\d+)", line)
        if m and (int(m.group(1)) or int(m.group(2))): capfull.append(f"{lid} ({sp['kind']}, {','.join(stops) or '-'}): own {m.group(1)}, can-draw {m.group(2)} dropped on the card")
        harvests.append(h)
    print(f"sim_harvest: {len(legs)} legs simulated (plan {plan['fingerprint']}, {plan['table_entries']} table entries)")
    args = py + [str(HERE / "refill_pack.py"), "--package", str(refill_pkg), "--source", str(src), "--out", str(out / "refill"), "--tool-cache", str(tools),
                 "--harvest-files", "lists-only", "--closure", "report", "--list", "package", "--stagewarm", "union", "--old-pack", "off", "--prune", a.prune]
    for h in harvests: args += ["--harvest", str(h)]
    r = subprocess.run(args, capture_output=True, text=True)
    rep = (out / "refill" / "REFILL-REPORT.txt").read_text() if (out / "refill" / "REFILL-REPORT.txt").exists() else r.stdout + r.stderr
    for ln in rep.splitlines():
        if ln.startswith(("stage-list merge", "vgx_stagewarm.bin tags", "RESULT:", "draw evidence", "prune:")): print(ln[:600])
        elif ln.startswith("vgx_stagewarm.bin (--stagewarm"): print(ln[:ln.find("; package")] if "; package" in ln else ln[:400])
    caps = re.findall(r"CAP: [^;]*", rep)
    for c in caps: print("  " + c)
    for c in capfull: print("  game-side cap on a leg's card: " + c)
    print(f"sim_harvest: refill exit {r.returncode}; CAP failures in the merge: {len(caps)}; legs whose card reached a game cap: {len(capfull)}")
    if not a.keep:
        shutil.rmtree(out / "legs", ignore_errors=True); shutil.rmtree(out / "refill" / "shadercache", ignore_errors=True)
    return 3 if caps or capfull else 0

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""collect.py - gather the legs of a seedwalk into ONE harvest folder (plan-perf31 A.3 / A1). Python 3.8+, run with python3 -I. Reads OUT/legs/*, writes OUT/harvest.

  collect.py collect OUT [--out DIR] [--allow-incomplete] [--strict]
  collect.py selftest        (the same checks run inside `plan.py selftest`)

Harvest folder (the interface the merge gate, tools/pack24/refill_pack.py's emulator-folder input, reads; see README.md):
  MANIFEST.json     format, plan fingerprint, per-leg status, eboot sha256 + seed id of every leg, profile words, program counts, conflicts, coverage per pass
  programs/<key>/v3/{v,f}/HH/HASH.gxp     the union of the legs' compiled shaders (vitaGL cache layout, <key> without the GXV_SHADER_CACHE_TAG suffix)
  lists/<leg>/{vgx_stagewarm.bin,vgx_stagewarm.bin.jnl,vgx_stagewarm.bin.ply,vgx_shaderlist.bin}   each leg's learned lists (package records + what the leg learned),
                    as written; the journal (.jnl: what the leg learned since the stage file's last append, e.g. in its last seconds) and a complete .tmp of the
                    journal or the player file (a write whose rename was cut) are copied too: the merge reads them as the game does
  lines/<leg>.txt   the log lines the gate reads: cover:, profile, seedwalk:, pack:, pack-miss:, first-use:, flow: entry ... done, survey: ... done
  cover.tsv         every cover: line, one row per line: leg, pass, then its key=value fields as columns
  cover-census.md   stage x {links in play, compiles in play, misses, records learned, can-draw records learned, uploads} (plan-perf31 A.5 item 4; the retail profile in
                    its header). new_recs / new_candraw: what the visit learned (fix31: start-up loads are not counted)
Checks (exit 3 when any fails; --allow-incomplete downgrades the legs' completeness, nothing else):
  * every leg's env.txt passes the allow-list (a text switch or a forced pack in ANY leg refuses the harvest);
  * the same program name from two legs has the same bytes (a difference = the compiler is not deterministic, or a text changed under one name);
  * all legs carry the same profile word (when the game logs one; --strict: every leg must), all legs ran the same eboot and the same seed;
  * every stage-table entry was finished once or more in every pass of the plan (missing / duplicate stops are listed).
Nothing is written outside the harvest folder (it is replaced as a whole: written to <harvest>.tmp, then renamed).
"""
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

def load_plan_module():
    spec = importlib.util.spec_from_file_location("seedwalk_plan", HERE / "plan.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules["seedwalk_plan"] = m
    spec.loader.exec_module(m)
    return m

GXP = re.compile(r"^([0-9A-Fa-f]{8})(-[A-Za-z0-9]{1,12})?$")
KEEP_LINES = re.compile(r"\bcover:|\bprofile\b|\bseedwalk:|\bpack:|pack-miss:|first-use:|flow: entry \d+ of \d+|flow: done|survey: \d+ hops done|powerup-test:|boot: play began|stage-room")
KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")
# the card's list files a leg leaves (fix31: the stage journal and the complete temporary files too; the merge reads a complete .tmp first, as the game does)
# (harvprep31) and the draw evidence of a harvest leg (GXV_SEEDWALK=1: vgx_stagewarm.bin.drawn, magic VGXSTD01: every distinct draw the leg made, held
# records included), which tools/pack24/refill_pack.py prunes an over-full stage by
LIST_FILES = ("vgx_stagewarm.bin", "vgx_stagewarm.bin.jnl", "vgx_stagewarm.bin.jnl.tmp", "vgx_stagewarm.bin.ply", "vgx_stagewarm.bin.ply.tmp",
              "vgx_stagewarm.bin.drawn", "vgx_stagewarm.bin.drawn.tmp", "vgx_shaderlist.bin")

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

def count_records(p, head=8, rec=24):
    try:
        n = Path(p).stat().st_size
    except OSError:
        return None
    return (n - head) // rec if n >= head and (n - head) % rec == 0 else -1

def collect(out, harvest=None, allow_incomplete=False, strict=False):
    P = load_plan_module()
    out = Path(out)
    plan = json.loads((out / "plan.json").read_text())
    H = Path(harvest) if harvest else out / "harvest"
    tmp = H.with_name(H.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    (tmp / "programs").mkdir(parents=True)
    (tmp / "lists").mkdir()
    (tmp / "lines").mkdir()
    problems, warnings = [], []
    legs, programs, conflicts = {}, {}, []
    profile_words, ebshas, seeds = {}, {}, {}
    cover_rows, cover_cols = [], []
    done_by_pass = {}
    for lid in plan["legs"]:
        ld = out / "legs" / lid
        if not (ld / "spec.json").exists():
            problems.append(f"{lid}: no spec.json")
            continue
        spec = json.loads((ld / "spec.json").read_text())
        st = {}
        if (ld / "status.json").exists():
            st = json.loads((ld / "status.json").read_text())
        info = {}
        if (ld / "run-info.json").exists():
            try:
                info = json.loads((ld / "run-info.json").read_text())
            except ValueError:
                pass
        ran = (ld / "run" / "log.txt").exists() or (ld / "card").is_dir()
        entry = {"kind": spec["kind"], "pass": spec["pass"], "stops": len(spec["stops"]), "ran": ran, "complete": bool(st.get("complete")), "done": (ld / "DONE").exists(),
                 "stops_done": st.get("stops_done", []), "stops_missing": st.get("stops_missing", spec["stops"] if not ran else []), "ebsha": info.get("ebsha"),
                 "seed": info.get("seed"), "wall_s": info.get("wall_s"), "guest_faults": st.get("guest_faults"), "fatal_lines": st.get("fatal_lines", [])}
        legs[lid] = entry
        if not ran:
            continue
        # the env the leg really used (run.sh appends the emulator's keys), against the allow-list
        for f in (ld / "env.txt", ld / "run" / "env.txt"):
            if f.exists():
                for b in P.check_env_text(P.read_text(f)):
                    problems.append(f"{lid}: {f.relative_to(out)}: REFUSED: {b}")
        if not entry["complete"] and not allow_incomplete:
            # (harvprep31) a leg whose missing stops its follow-up legs (<id>-rN, plan.py resume) finished is covered: its own lists and lines are still used
            fol = [x for x in plan["legs"] if re.fullmatch(re.escape(lid) + r"-r\d+", x)]
            fdone = set()
            for x in fol:
                try:
                    fst = json.loads((out / "legs" / x / "status.json").read_text())
                except (OSError, ValueError):
                    continue
                fdone |= set(fst.get("stops_done", []))
            if fol and entry["stops_missing"] and set(entry["stops_missing"]) <= fdone:
                warnings.append(f"{lid}: incomplete, its missing stops {entry['stops_missing']} were finished by {', '.join(fol)}")
            else:
                problems.append(f"{lid}: incomplete (missing {entry['stops_missing']}); plan.py resume OUT makes a follow-up leg")
        if entry["fatal_lines"]:
            warnings.append(f"{lid}: log lines of a failing kind: {entry['fatal_lines']}")
        if info.get("ebsha"):
            ebshas.setdefault(info["ebsha"], []).append(lid)
        if info.get("seed"):
            seeds.setdefault(info["seed"], []).append(lid)
        for s in entry["stops_done"]:
            done_by_pass.setdefault(spec["pass"], []).append(s)
        # the log's lines
        text = P.read_text(ld / "run" / "log.txt")
        keep = []
        for n, ln in enumerate(text.split("\n"), 1):
            if KEEP_LINES.search(ln):
                keep.append(ln[:400])
            m = P.RE_COVER.search(ln)
            if m:
                kv = dict(KV.findall(m.group(1)))
                for k in kv:
                    if k not in cover_cols:
                        cover_cols.append(k)
                cover_rows.append((lid, spec["pass"], n, kv))
        (tmp / "lines" / f"{lid}.txt").write_text("\n".join(keep) + ("\n" if keep else ""))
        for w in st.get("profile_words", []):
            profile_words.setdefault(w, []).append(lid)
        if not st.get("profile_words"):
            (warnings if not strict else problems).append(f"{lid}: no profile word in the log (the game's stamp line is not there)")
        # the card: lists, then compiled files
        card = ld / "card"
        if card.is_dir():
            for name in LIST_FILES:
                if (card / name).exists():
                    d = tmp / "lists" / lid
                    d.mkdir(exist_ok=True)
                    shutil.copyfile(card / name, d / name)
            for kdir in sorted(p for p in card.iterdir() if p.is_dir() and GXP.match(p.name)):
                key = kdir.name.split("-")[0].lower()          # <key>-<tag> -> <key>
                for p in sorted(kdir.rglob("*.gxp")):
                    rel = p.relative_to(kdir).as_posix()
                    full = f"{key}/{rel}"
                    digest = sha(p)
                    if full in programs:
                        if programs[full][0] != digest:
                            conflicts.append({"program": full, "legs": [programs[full][1], lid]})
                        continue
                    programs[full] = (digest, lid)
                    dst = tmp / "programs" / full
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(p, dst)
    for c in conflicts:
        problems.append(f"conflict: {c['program']} has other bytes in {c['legs'][1]} than in {c['legs'][0]}")
    if len(profile_words) > 1:
        problems.append(f"profile words differ between legs: {sorted(profile_words)} (the harvest is not one shader profile)")
    if len(ebshas) > 1:
        problems.append(f"legs ran {len(ebshas)} different eboot builds: {sorted(h[:12] for h in ebshas)}")
    if len(seeds) > 1:
        problems.append(f"legs started from {len(seeds)} different seeds: {sorted(seeds)}")
    # coverage: every table entry finished in every pass of the plan
    table = plan["table"]
    coverage = {}
    for p in sorted({lg["pass"] for lg in legs.values()}):
        got = done_by_pass.get(p, [])
        miss = [s for s in table if s not in got]
        dup = sorted({s for s in got if got.count(s) > 1})
        coverage[p] = {"entries": len(table), "finished": len(set(got) & set(table)), "missing": miss, "twice": dup}
        if miss:
            (warnings if allow_incomplete else problems).append(f"pass {p}: {len(miss)} stage-table entries not finished: {miss[:6]}{' ...' if len(miss) > 6 else ''}")
        if dup:
            warnings.append(f"pass {p}: finished twice (a follow-up leg repeated them): {dup[:6]}")
    # cover.tsv and the census
    cols = ["leg", "pass", "logline"] + cover_cols
    with open(tmp / "cover.tsv", "w") as f:
        f.write("\t".join(cols) + "\n")
        for lid, ps, n, kv in cover_rows:
            f.write("\t".join([lid, ps, str(n)] + [kv.get(c, "") for c in cover_cols]) + "\n")
    write_census(tmp / "cover-census.md", plan, cover_rows, profile_words, ebshas, seeds)
    sizes = {n: count_records(p) for lid in legs for n, p in [(lid, out / "legs" / lid / "card" / "vgx_stagewarm.bin")] if p.exists()}
    man = {"format": 1, "plan": plan["fingerprint"], "table_entries": len(table), "legs": legs, "profile_words": sorted(profile_words), "eboots": {k[:16]: v for k, v in ebshas.items()},
           "seeds": sorted(seeds), "programs": len(programs), "conflicts": len(conflicts), "coverage": coverage, "cover_lines": len(cover_rows),
           "stage_list_records": sizes, "warnings": warnings, "problems": problems, "ok": not problems}
    (tmp / "MANIFEST.json").write_text(json.dumps(man, indent=1))
    if H.exists():
        shutil.rmtree(H)
    os.replace(tmp, H)
    return man, H

def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None

def write_census(path, plan, rows, profile_words, ebshas, seeds):
    """stage x {links in play, compiles in play, misses, new records, uploads in play}: summed over the visits found (the last visit's line wins per leg/stage)"""
    agg = {}
    for lid, ps, n, kv in rows:
        stage = kv.get("stage") or kv.get("name") or "?"
        sc = kv.get("scenario", "")
        key = (ps, stage, sc)
        a = agg.setdefault(key, {"links": 0, "compiles": 0, "misses": 0, "newrec": 0, "newcd": 0, "uploads": 0, "visits": 0, "worst": 0.0})
        a["visits"] += 1
        for f, k in (("links", "links"), ("compiles", "compiles"), ("misses", "misses"), ("newrec", "new_recs"), ("newcd", "new_candraw"), ("uploads", "uploads")):
            v = num(kv.get(k, kv.get("newrec") if k == "new_recs" else None))   # (the game's key is new_recs; "newrec" was the draft interface's)
            if v is not None:
                a[f] += int(v)
        w = num(kv.get("worst_ms", kv.get("link_worst_ms")))
        if w is not None:
            a["worst"] = max(a["worst"], w)
    with open(path, "w") as f:
        f.write("# Cover census (seedwalk harvest)\n\n")
        f.write("Profile: retail (no shader-text switch set; the walk's env passes the allow-list). Profile word(s): %s. Build(s): %s. Seed(s): %s. Plan %s.\n\n"
                % (", ".join(sorted(profile_words)) or "not logged", ", ".join(k[:12] for k in ebshas) or "?", ", ".join(sorted(seeds)) or "?", plan["fingerprint"]))
        f.write("Emulator counts are meaningful, times are not (software GL on shared cores).\n\n")
        f.write("| pass | stage | scenario | visits | links in play | compiles in play | misses | records learned | can-draw learned | uploads in play | worst link ms |\n"
                "|---|---|---|---|---|---|---|---|---|---|---|\n")
        for (ps, stage, sc), a in sorted(agg.items()):
            f.write(f"| {ps} | {stage} | {sc} | {a['visits']} | {a['links']} | {a['compiles']} | {a['misses']} | {a['newrec']} | {a['newcd']} | {a['uploads']} | {a['worst']:.1f} |\n")
        if not agg:
            f.write("| (no cover: line in any log: the game build has no cover: line yet) | | | | | | | | | | |\n")

def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__); return 0
    if argv[1] == "selftest":
        sys.path.insert(0, str(HERE))
        return load_plan_module().selftest()
    if argv[1] != "collect" or len(argv) < 3:
        print(__doc__); return 1
    out, h, allow, strict = argv[2], None, False, False
    rest = argv[3:]
    it = iter(rest)
    for x in it:
        if x == "--out": h = next(it)
        elif x == "--allow-incomplete": allow = True
        elif x == "--strict": strict = True
        else: raise SystemExit(f"collect.py: unknown option {x}")
    man, H = collect(out, h, allow, strict)
    print(f"harvest {H}: {man['programs']} programs, {man['cover_lines']} cover lines, {len(man['legs'])} legs, {man['conflicts']} conflicts, profile words {man['profile_words'] or 'none logged'}")
    for w in man["warnings"]:
        print("  warning:", w)
    for p in man["problems"]:
        print("  PROBLEM:", p)
    return 0 if man["ok"] else 3

if __name__ == "__main__":
    sys.exit(main(sys.argv))

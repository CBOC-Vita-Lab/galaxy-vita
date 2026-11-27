#!/usr/bin/env python3
"""sim_pull.py: make a SIMULATED pull_tree result (what pull_tree writes for ux0:data/smg/shadercache after a refill) so that refill_pack.py can be
tested on the host. The shader files are fake but pass ShaderPack::StageImage (u32 0 + a 160-byte GXP shell); their names are the real ones:
the texts the generator makes for the given warm-up list (tools/listnames, run with the tree's defaults by refill_pack's functions).

  python3 -I sim_pull.py --list <vgx_shaderlist.bin> --source <tree> --out <new folder> [--extra N] [--skip K] [--tour tour_legs.txt]
        [--ply N] [--no-stages] [--tgz] [--no-recipe N] [--profile-tool BIN] [--profile-env K=V ...] [--fake-salt S]
  --extra N   N more fake files (aux programs / tour-only texts)     --skip K   leave K of the used names out (a leg that never ran)
  --ply N     N player records in vgx_stagewarm.bin.ply              --no-stages  no stage records for the last 3 tour stages
  seedgate31: the stage records hold the REAL key hashes of the list (listnames --closure), so a sim pull is a closed seed unless a switch opens it:
  --skip K leaves K compiled files out (records then lack files), --no-recipe N makes N records with hashes the list does not have,
  --profile-tool BIN (tests/vgx_profile_word's "print") + --profile-env GXV_VSOUT=0 ... writes <out>/run.log with the game's profile lines for that environment,
  --fake-salt S changes the fake files' bytes (a harvest "compiled by another compiler": the same names, other bytes).
  fix31, an emulator LEG's card as the game leaves it (tools/seedwalk; sim_harvest.py makes a whole walk of them):
  --base-stage FILE   the card's stage list starts with FILE's own records (a leg starts from the candidate seed: StageLoadPackaged adds the package's own records
                      and the flush writes them to the card; its can-draw records and stamp never go there). The game's caps apply (2,048 own records a stage,
                      32,768 in all; 1,024 / 16,384 can-draw): what they drop is counted as the leg's "cap-full" lines.
  --stops S:N,...     the leg's visits (stage name : scenario 1..7; 0 = a menu visit, learned under MENU unhinted). For each visit the leg learns draws of the
                      stage's pool of new draws (--pool-frac of the stage's base records, at least --pool-min; real key pairs of --list, so closure holds) with
                      the game's labels (RecordLabel): the visit's scenario hint, or one unhinted copy when the same card saw the draw in another scenario;
                      --slow-frac of them also as a slow-tagged copy; --candraw-pool N can-draw records (GXV_STAGE_ENUM) of a pool of N a stage, hinted.
  --take F            share of a stage's pool one visit learns (default 0.7)  --jnl-frac F  the last share of a leg's learned records is in the journal only
  --no-files          no compiled files (a lists-only harvest; --list is hard-linked, not copied)
  harvprep31: a leg runs with GXV_SEEDWALK=1, so its card keeps up to 8,192 own records a stage (--old-cap: the release's 2,048, a build before harvprep31), and it
  leaves its draw evidence, vgx_stagewarm.bin.drawn (VGXSTD01): every distinct draw of the leg. --pkg-drawable F: the share of the package's records that can be
  drawn at all (a fixed set by hash; the rest are stale: other generators, screens no walk reaches); each visit draws --draw-take of its stage's drawable records,
  every leg --draw-take of the menu's and the player list's (title, pause menu, star select), plus everything it learned. --no-evidence: no .drawn file.
Output: <out>/shadercache/<key>/v3/{v,f}/HH/HASH.gxp, vgx_shaderlist.bin (the --list file), vgx_stagewarm.bin, vgx_stagewarm.bin.ply [, .jnl] [, <out>.tgz]
The files are test data: never ship them.
"""
import argparse, importlib.util, os, random, struct, subprocess, sys, tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("rp", HERE / "refill_pack.py"); rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)

def fake_record(kind, seed):
    rnd = random.Random(seed)
    size = 160 + 4 * rnd.randint(0, 120)
    g = bytearray(size)
    g[0:4] = b"GXP\0"
    struct.pack_into("<I", g, 8, size)
    struct.pack_into("<I", g, 20, 0 if kind == "v" else 1)
    struct.pack_into("<I", g, 36, 0)      # no parameters
    struct.pack_into("<I", g, 40, 0)
    for i in range(156, size): g[i] = rnd.randint(0, 255)
    return struct.pack("<I", 0) + bytes(g)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", required=True); ap.add_argument("--source", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--extra", type=int, default=0); ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--tour", default=str(HERE / "tour_legs.txt")); ap.add_argument("--ply", type=int, default=7)
    ap.add_argument("--no-stages", action="store_true"); ap.add_argument("--tgz", action="store_true")
    ap.add_argument("--listnames", required=True, help="a built tools/pack24 listnames binary")
    ap.add_argument("--no-recipe", type=int, default=0); ap.add_argument("--profile-tool"); ap.add_argument("--profile-env", action="append", default=[])
    ap.add_argument("--fake-salt", type=int, default=0)
    ap.add_argument("--base-stage"); ap.add_argument("--stops", default=""); ap.add_argument("--pool-frac", type=float, default=0.5)
    ap.add_argument("--pool-min", type=int, default=40); ap.add_argument("--take", type=float, default=0.7); ap.add_argument("--slow-frac", type=float, default=0.05)
    ap.add_argument("--candraw-pool", type=int, default=0); ap.add_argument("--jnl-frac", type=float, default=0.1); ap.add_argument("--no-files", action="store_true")
    ap.add_argument("--menu-pool", type=int, default=400)
    ap.add_argument("--old-cap", action="store_true"); ap.add_argument("--pkg-drawable", type=float, default=0.75); ap.add_argument("--draw-take", type=float, default=0.7)
    ap.add_argument("--no-evidence", action="store_true")
    a = ap.parse_args()
    tree = Path(a.source); out = Path(a.out)
    if out.exists(): raise SystemExit(f"{out} exists")
    env, _ = rp.entry_defaults(tree)
    runenv = dict({k: v for k, v in env.items() if k.startswith("GXV_")}, PATH=os.environ["PATH"])
    clo = subprocess.run([a.listnames, a.list, "--closure"], capture_output=True, text=True, env=runenv).stdout.splitlines()
    kv, kf = [], []
    for ln in clo:
        kind, hx, nm = ln.split()
        (kv if kind == "v" else kf).append((int(hx, 16), nm))
    kv.sort(); kf.sort()
    names = sorted({nm for _, nm in kv + kf})      # (every text of the list: a closed seed has all of them)
    identity = rp.identity_from_source(tree); key = rp.fnv1a(identity)
    v3 = out / "shadercache" / key / "v3"
    names = sorted(names)[: len(names) - a.skip] if a.skip else names
    rnd = random.Random(24)
    extra = []
    for i in range(a.extra):
        kind = "f" if i % 5 else "v"; h = (rnd.getrandbits(40) << 24) | rnd.getrandbits(24)
        extra.append(f"{kind}/{h >> 56:02X}/{h:X}.gxp")
    for i, n in enumerate([] if a.no_files else names + extra):
        p = v3 / n; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(fake_record(n[0], i + 100000 * a.fake_salt))
    sc = out / "shadercache"; sc.mkdir(parents=True, exist_ok=True)
    if a.no_files:
        try: os.link(a.list, sc / "vgx_shaderlist.bin")
        except OSError: (sc / "vgx_shaderlist.bin").write_bytes(Path(a.list).read_bytes())
    else: (sc / "vgx_shaderlist.bin").write_bytes(Path(a.list).read_bytes())
    if a.base_stage:
        return leg_card(a, sc, kv, kf, out)
    nrec = [0]
    def pair():
        # walk the list's keys so that every vertex and every fragment key is in some record (closure: all their files are then needed)
        i = nrec[0]; nrec[0] += 1
        return kv[i % len(kv)][0], kf[(i * 7 + i // len(kf)) % len(kf)][0]
    recs = []
    for lb in (0x12c00, 0x16400):
        v, f = pair(); recs.append((rp.MENU_KEY, lb, v, f))
    stages = rp.tour_stage_names(a.tour)
    for j, n in enumerate(stages[:-3] if a.no_stages else stages):
        for k in range(1 + j % 4):
            v, f = pair(); recs.append((rp.stage_key_of(n), 0x16400 + k, v, f))
    for i in range(len(kf)):                          # (every fragment key in at least one record)
        v, f = kv[i % len(kv)][0], kf[i][0]; recs.append((rp.stage_key_of(stages[0]), 0x1A000 + i, v, f))
    for i in range(a.no_recipe): recs.append((rp.stage_key_of(stages[1]), 0x1B000 + i, 0xDEAD0000 + i, 0xBEEF0000 + i))
    (sc / "vgx_stagewarm.bin").write_bytes(b"VGXSTW01" + b"".join(struct.pack("<IIQQ", *r) for r in recs))
    ply = [(rp.PLAYER_KEY, 0x16400, kv[i % len(kv)][0], kf[(i * 3) % len(kf)][0]) for i in range(a.ply)]
    body = b"".join(struct.pack("<IIQQ", *r) for r in ply)
    h = 2166136261
    for b in body: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    (sc / "vgx_stagewarm.bin.ply").write_bytes(b"VGXPLY01" + struct.pack("<II", len(ply), h) + body)
    if a.profile_tool:
        penv = dict(os.environ, **dict(x.split("=", 1) for x in a.profile_env))
        (out / "run.log").write_text(subprocess.run([a.profile_tool, "print"], capture_output=True, text=True, env=penv).stdout)
    print(f"sim: identity {identity!r} key {key}: {len(names)} list files + {len(extra)} extra, {len(recs)} stage records, {len(ply)} player records -> {out}")
    if a.tgz:
        with tarfile.open(str(out) + ".tgz", "w:gz") as t: t.add(sc, arcname="shadercache")
        print("tgz:", str(out) + ".tgz")

def leg_card(a, sc, kv, kf, out):
    """(fix31) the card of one emulator leg: the package's own records, then what the leg learned, as the game labels and caps them"""
    sys.path.insert(0, str(HERE.parent / "seedrun")); import stagetags as T
    base = [r for r in T.read(a.base_stage) if r[0] != T.STAMP_KEY and not r[1] & T.CANDRAW]
    per_base = {}
    for r in base: per_base[r[0]] = per_base.get(r[0], 0) + 1
    card, own, cd = [], {}, {}       # (the game's sets: own records a stage {(vs, fs, lb)}, can-draw ones)
    capfull = {"own": 0, "cd": 0}
    def add(st, lb, vs, fs, book, limit, total):
        s_ = book.setdefault(st, set())
        if (vs, fs, lb) in s_: return False
        if sum(len(x) for x in book.values()) >= total or len(s_) >= limit: capfull["own" if book is own else "cd"] += 1; return False
        s_.add((vs, fs, lb)); card.append((st, lb, vs, fs)); return True
    own_cap = rp.STAGE_MAX_RECS if a.old_cap else rp.STAGE_MAX_RECS_HARVEST   # (harvprep31: GXV_SEEDWALK=1 lifts the per-stage cap in a leg)
    for st, lb, vs, fs in base: add(st, lb, vs, fs, own, own_cap, rp.STAGE_MEM_MAX)
    nbase = len(card)
    def mix(*xs):
        h = 1469598103934665603
        for x in xs: h = ((h ^ (x & 0xFFFFFFFFFFFFFFFF)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return h
    learned = []
    seen = {}       # (stage, vs, fs, draw) -> labels the card holds (RecordLabel's haveAny / haveCur / haveOther)
    # (harvprep31) the draw evidence: the package's drawable records a visit draws (a fixed drawable set by hash; a random share of it each visit), then all learned
    drawn = {}
    by_stage = {}
    for r in base: by_stage.setdefault(r[0], []).append(r)
    ev_rnd = random.Random(mix(len(a.stops), sum(ord(c) for c in a.stops)))
    def draw_pkg(st):
        for st_, lb, vs, fs in by_stage.get(st, ()):
            if (mix(st_, vs, fs, lb & T.DRAW) % 1000) >= a.pkg_drawable * 1000: continue
            if ev_rnd.random() < a.draw_take: drawn.setdefault((st_, vs, fs, lb & T.DRAW), (st_, lb & T.DRAW, vs, fs))
    draw_pkg(rp.MENU_KEY); draw_pkg(rp.PLAYER_KEY)
    for stop in [x for x in a.stops.split(",") if x]:
        name, sc_ = stop.rsplit(":", 1); cur = int(sc_)
        st = rp.MENU_KEY if cur == 0 else rp.stage_key_of(name)
        draw_pkg(st)
        pool = a.menu_pool if cur == 0 else max(a.pool_min, int(a.pool_frac * per_base.get(st, 0)))
        rnd = random.Random(mix(st, cur, len(learned)))
        for j in range(pool):
            if rnd.random() >= a.take: continue
            h = mix(st, j)
            vs, fs, draw = kv[h % len(kv)][0], kf[(h >> 20) % len(kf)][0], 0x1E000 | (j & 0xFFF)
            drawn.setdefault((st, vs, fs, draw), (st, draw, vs, fs))
            have = seen.setdefault((st, vs, fs, draw), set())
            if 0 in have: continue                                   # (RecordLabel: the list holds it for any scenario)
            if cur == 0 or cur > 7: lab = 0
            elif cur in have: continue
            else: lab = 0 if have else cur
            have.add(lab)
            if add(st, draw | (lab << T.SCEN_SHIFT), vs, fs, own, own_cap, rp.STAGE_MEM_MAX): learned.append(card[-1])
            if rnd.random() < a.slow_frac and add(st, draw | T.SLOW | (lab << T.SCEN_SHIFT), vs, fs, own, own_cap, rp.STAGE_MEM_MAX): learned.append(card[-1])
        for j in range(a.candraw_pool if cur else 0):
            if rnd.random() >= a.take: continue
            h = mix(st, j, 77)
            vs, fs, draw = kv[h % len(kv)][0], kf[(h >> 20) % len(kf)][0], 0x1D000 | (j & 0xFFF)
            if add(st, draw | T.CANDRAW | (cur << T.SCEN_SHIFT), vs, fs, cd, rp.CANDRAW_STAGE_MAX, rp.CANDRAW_MEM_MAX): learned.append(card[-1])
    # the stage file holds the base and the learned records the last flush wrote; the rest (the last --jnl-frac of what was learned) is in the journal only
    njnl = int(len(learned) * a.jnl_frac)
    infile = card[:len(card) - njnl] if njnl else card
    jnl = card[len(card) - njnl:] if njnl else []
    (sc / "vgx_stagewarm.bin").write_bytes(b"VGXSTW01" + b"".join(struct.pack("<IIQQ", *r) for r in infile))
    def side(path, magic, recs):
        body = b"".join(struct.pack("<IIQQ", *r) for r in recs)
        h = 2166136261
        for b in body: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
        path.write_bytes(magic + struct.pack("<II", len(recs), h) + body)
    if jnl: side(sc / "vgx_stagewarm.bin.jnl", b"VGXSTJ01", jnl)
    if not a.no_evidence: side(sc / "vgx_stagewarm.bin.drawn", b"VGXSTD01", list(drawn.values()))
    side(sc / "vgx_stagewarm.bin.ply", b"VGXPLY01", [(rp.PLAYER_KEY, 0x16400, kv[i % len(kv)][0], kf[(i * 3) % len(kf)][0]) for i in range(a.ply)])
    if a.profile_tool:
        penv = dict(os.environ, **dict(x.split("=", 1) for x in a.profile_env))
        (out / "run.log").write_text(subprocess.run([a.profile_tool, "print"], capture_output=True, text=True, env=penv).stdout)
    print(f"leg card: {nbase} package records, {len(learned)} learned ({sum(1 for r in learned if r[1] & T.CANDRAW)} can-draw, "
          f"{sum(1 for r in learned if r[1] & T.SCEN)} hinted, {sum(1 for r in learned if r[1] & T.SLOW)} slow), {len(jnl)} in the journal only, "
          f"{0 if a.no_evidence else len(drawn)} distinct draws in the evidence; "
          f"game cap-full drops: own {capfull['own']}, can-draw {capfull['cd']} -> {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

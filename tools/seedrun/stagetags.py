#!/usr/bin/env python3
"""stagetags (seedrun31 A3/A4): the tags of a packaged stage list (vgx_stagewarm.bin, magic VGXSTW01, 24-byte records {u32 stage, u32 lb, u64 vs, u64 fs}).

The record word lb (platform/vgx/src/vgx_stage_first_visit.h):
  bits 0..24  the draw (blend key 0..12, vertex layout mask 13..24)
  bit  25     can draw: enumerated from the stage's materials, never seen drawn (a tier of its own at run time, behind the queue)
  bits 26..27 menusplit's class (menu records)
  bit  28     slow: queued first at the stage's next load (when its program is not linked)
  bits 29..31 scenario hint 1..7 (0 = any)
The renderer has no built-in priority hashes or player seed any more: what C182/C183 and firstuse24 hard-coded is data made here.

  stagetags.py legacy IN OUT [--ply FILE ...]   what the renderer had built in until seedrun31, as data (see legacy()): the nine C182/C183 priority records get
                                                 the slow tag (exact stage key, 64-bit keys and draw word); the old player seed's eleven low-32-bit entries become
                                                 PLYR records at the end (each half resolved to the one key of IN / the .ply files that ends in it); a stamp record.
                                                 tools/pack24/refill_pack.py runs it on every stage list it writes.
  stagetags.py vpk IN.vpk OUT.vpk                the same on a VPK's shadercache/vgx_stagewarm.bin (every other member copied as it is).
  stagetags.py check IN                          exit 1 when IN is not ready for a package (no stamp, or legacy would still change it).
  stagetags.py checkvpk IN.vpk                   (fix31) the same on the VPK's shadercache/vgx_stagewarm.bin; also exit 1 when the VPK has none, or two. The hard
                                                 check of every release VPK (docs/RELEASE-3.0-CHECKLIST.md; tools/vgx_build/build.py --seed runs it).
  stagetags.py package BASE.vpk SEED OUT.vpk [--loose]   (fixnow31: the seed's shaders go through c182_compact_seed.compact(strict=True) into one v3/seed.c182,
                                                 so the VPK has about 14 entries; checkvpk then fails on more than 64 members, on loose v3/[vf]/ files or on a
                                                 missing seed.c182. --loose (package and checkvpk): the old layout, for the selftest's stub seed only.)  (fix31) the release VPK: BASE's members (the CMake smg-vgx.vpk: eboot.bin, sce_sys/) without any shadercache/,
                                                 then SEED's shadercache/ (SEED: a VPK, or a folder that is or holds shadercache/), its stage list through
                                                 legacy(); then checkvpk on OUT, which is removed when it fails (exit 1).
  stagetags.py normalize IN OUT                  one record a draw where hints disagree: a draw recorded unhinted and hinted, or with two scenarios, keeps
                                                 one unhinted record (at the first one's place; slow kept if any copy was slow); can-draw records of a draw the
                                                 stage's own records hold are dropped, and can-draw copies collapse the same way. Record order is kept.
  stagetags.py show IN                           per stage: records, slow, hinted (by scenario), can-draw.
  stagetags.py synth OUT                         a list shaped like the C181 test19 one around the nine records (tests: tools/test_c182_firstvisit.sh).
  stagetags.py selftest
"""
import re
import struct
import sys
from pathlib import Path

MAGIC = b"VGXSTW01"
PLY_MAGIC = b"VGXPLY01"
DRAW = 0x1FFFFFF
CANDRAW = 1 << 25
MENUCLS = 3 << 26
SLOW = 1 << 28
SCEN_SHIFT = 29
SCEN = 7 << SCEN_SHIFT
PLAYER_KEY = 0x504C5952
MENU_KEY = 0x4D454E55

# C182 / C183 (docs/c182-first-visit.md): console frames 7665/7669/7728 (Observatory, 2290-c106-look-m5) and 2350/2380 (Honeyhive, 3227-w181-P-M08).
# Exactly what vgx_stage_first_visit.h Historical() matched until seedrun31: (stage, vs, fs, draw word).
HISTORICAL = [
    (0x4903D024, 0x671B7A9FAE441691, 0x0D14F45BAE134E25, 0x16C00), (0x4903D024, 0x671B7A9FAE441691, 0x85DA8833CA90CC24, 0x16C00),
    (0x4903D024, 0x1C119354708911FF, 0x854159D5C0A23060, 0x16C00), (0x4903D024, 0x1C119354708911FF, 0xB0119FCE9EA638A1, 0x16C00),
    (0x4903D024, 0x7ED555EA91ECD182, 0x8EC5794F6B01CBAF, 0x16400), (0x4903D024, 0x1FB3FA05B81B6E91, 0xEC49BBEF9D0C8383, 0x16551),
    (0x17BAB031, 0x166A6A6B5C0367F7, 0x11C015BFCB16A96C, 0x1C551), (0x17BAB031, 0xEFB2F05617D51094, 0x6652CBB3C18517DE, 0x1016400),
    (0x17BAB031, 0xE94D45D78111DCEE, 0x6652CBB3C18517DE, 0x16400),
]
# The built-in player seed (vgx_backend_gl.cpp kPlySeed until seedrun31): low 32 bits of the key hashes, draw word.
PLY_SEED = [
    (0x6891D205, 0xF30E1197, 0x12C00), (0x464DAA45, 0x57115485, 0x16400), (0x65A4C4BE, 0x0F0752CF, 0x16400), (0x92B0A9FC, 0x54CC606C, 0x16400),
    (0x65A4C4BE, 0xBB0428C1, 0x16400), (0x65A4C4BE, 0x4AAA1954, 0x16400), (0xE801E2E8, 0x94813DC2, 0x16400), (0x07D4A19F, 0x4F48E5E0, 0x16400),
    (0x89624582, 0x2AA6C7A6, 0x16451), (0x277812B5, 0xB24EC6E8, 0x16C00), (0x7303AA2C, 0x0F0660BD, 0x16C00),
]


def read(path):
    return parse(Path(path).read_bytes(), path)


def parse(d, path="(data)"):
    if d[:8] == MAGIC:
        if (len(d) - 8) % 24:
            raise SystemExit(f"stagetags: {path}: cut short ({len(d) - 8} bytes of records)")
        return [struct.unpack_from("<IIQQ", d, 8 + 24 * i) for i in range((len(d) - 8) // 24)]
    if d[:8] == PLY_MAGIC:
        n = struct.unpack_from("<I", d, 8)[0]
        return [(PLAYER_KEY,) + struct.unpack_from("<IIQQ", d, 16 + 24 * i)[1:] for i in range(n)]
    raise SystemExit(f"stagetags: {path}: not a VGXSTW01 / VGXPLY01 file")


def encode(recs):
    return MAGIC + b"".join(struct.pack("<IIQQ", st, lb & 0xFFFFFFFF, vs, fs) for st, lb, vs, fs in recs)


def write(path, recs):
    Path(path).write_bytes(encode(recs))


STAMP_KEY = 0x53544147            # 'STAG': the stamp record legacy() leaves (vgx_backend_gl.cpp StageLoadPackaged reads it; never a stage)
STAMP_VS = 0x5345454452554E31    # "SEEDRUN1"
STAMP_VERSION = 1


def stamp_of(recs):
    """the stamp record's (version, tagged, seed records) or None"""
    for st, lb, vs, fs in recs:
        if st == STAMP_KEY and vs == STAMP_VS: return lb, fs >> 32, fs & 0xFFFFFFFF
    return None


def legacy(recs, extra=()):
    """returns (recs, report). What the renderer did with built-in data until seedrun31, as data, so that a list loaded by this build queues exactly as before:
    1. Historical(): the slow tag on the nine records, each under the stage key it was hard-coded for (the same draw under another stage was never a priority);
    2. kPlySeed: matched at run time by the low 32 bits of each shader's key hash among the registered shaders, queued after every player record; here each half is
       resolved to the one full key hash of the list (and of `extra`) that ends in it, and the pair goes in as a PLYR record at the end of the file (the package's
       player records are the last of the player list at run time, so it is queued where the seed was). An entry whose halves do not resolve to exactly one key each
       is left out and reported (at run time it would have matched nothing or an arbitrary one of two);
    3. a stamp record (stage 'STAG', never a stage) that says the list was made by this tool; the renderer logs its absence.
    Idempotent: a second run tags nothing and adds nothing."""
    hist = {(st, v, f, w) for st, v, f, w in HISTORICAL}
    out, tagged, already = [], 0, 0
    for st, lb, vs, fs in recs:
        if st == STAMP_KEY: continue
        if (st, vs, fs, lb & DRAW) in hist and not lb & CANDRAW:
            if lb & SLOW: already += 1
            else: lb |= SLOW; tagged += 1
        out.append((st, lb, vs, fs))
    pool = [r for r in list(recs) + list(extra) if r[0] != STAMP_KEY]
    allVs, allFs = {r[2] for r in pool}, {r[3] for r in pool}
    have = {(st, vs, fs, lb) for st, lb, vs, fs in out}
    resolved, unresolved, added, present = 0, [], 0, 0
    for v32, f32, w in PLY_SEED:
        cv = [h for h in allVs if h & 0xFFFFFFFF == v32]
        cf = [h for h in allFs if h & 0xFFFFFFFF == f32]
        if len(cv) != 1 or len(cf) != 1:
            unresolved.append("%08x/%08x (%d vs, %d fs keys)" % (v32, f32, len(cv), len(cf))); continue
        resolved += 1
        vs, fs = cv[0], cf[0]
        if (PLAYER_KEY, vs, fs, w) in have: present += 1; continue
        out.append((PLAYER_KEY, w, vs, fs)); have.add((PLAYER_KEY, vs, fs, w)); added += 1
    slowTotal = sum(1 for r in out if r[1] & SLOW)
    seedTotal = sum(1 for r in out if r[0] == PLAYER_KEY and any((r[2] & 0xFFFFFFFF, r[3] & 0xFFFFFFFF, r[1] & DRAW) == e for e in PLY_SEED))
    out.append((STAMP_KEY, STAMP_VERSION, STAMP_VS, (slowTotal << 32) | seedTotal))
    found = {(st, vs, fs, lb & DRAW) for st, lb, vs, fs in out if lb & SLOW}
    missing = [h for h in HISTORICAL if h not in found]
    rep = (f"historical priority records: {tagged} tagged slow, {already} were tagged already, {len(HISTORICAL) - len(missing)} of {len(HISTORICAL)} present"
           f"{' (absent: ' + ', '.join('%08x %016x/%016x %05x' % m for m in missing) + ')' if missing else ''}; "
           f"old player seed: {resolved} of {len(PLY_SEED)} entries resolve to one key each, {added} PLYR records added at the end, {present} were there already"
           f"{' (not resolved: ' + ', '.join(unresolved) + ')' if unresolved else ''}; stamp record written")
    return out, rep


def check(recs):
    """problems (a list of texts) of a list meant for a package: no stamp, a historical record without its tag, a resolvable seed entry missing from the player records"""
    probs = []
    if not stamp_of(recs): probs.append("no stamp record: the list was not made by stagetags.py legacy (the renderer would run it without the first-visit priorities)")
    again, _ = legacy(recs)
    body = lambda rr: [r for r in rr if r[0] != STAMP_KEY]
    if body(again) != body(recs):
        diff = len(body(again)) - len(body(recs))
        probs.append(f"legacy would still change it ({sum(1 for a, b in zip(body(again), body(recs)) if a != b)} records retagged, {diff} added)")
    return probs


def vpk(src, dst):
    """a copy of the VPK src with its shadercache/vgx_stagewarm.bin passed through legacy(); every other member copied byte for byte, in order"""
    import zipfile
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w") as zo:
        n = 0
        for info in zi.infolist():
            data = zi.read(info)
            if info.filename.endswith("shadercache/vgx_stagewarm.bin"):
                r = parse(data, info.filename)
                out, rep = legacy(r)
                data = encode(out); n += 1
                print(f"vpk: {info.filename}: {rep}")
            zo.writestr(info, data)
    if n != 1: raise SystemExit(f"stagetags vpk: {src} has {n} shadercache/vgx_stagewarm.bin members (want 1)")


def vpk_list(path):
    """-> (records of the VPK's shadercache/vgx_stagewarm.bin, problems)"""
    import zipfile
    with zipfile.ZipFile(path) as z:
        names = [i.filename for i in z.infolist() if i.filename.endswith("shadercache/vgx_stagewarm.bin")]
        if len(names) != 1: return None, [f"{len(names)} shadercache/vgx_stagewarm.bin members (want 1): the package has {'no' if not names else 'more than one'} stage list"]
        return parse(z.read(names[0]), names[0]), []


MAX_MEMBERS = 64   # fixnow31: a compact release VPK holds about 14 entries (3 art/LiveArea groups, eboot, the seed's 4); thousands means loose shader files


def checkvpk(path, compact=True):
    """problems of a release VPK: the stage list (none, two, no stamp, or not as legacy() leaves it), and (fixnow31) the compact seed: more than MAX_MEMBERS
    entries, loose shadercache/<key>/v3/[vf]/ shader files, or no v3/seed.c182. The compact reader (platform/vgx/vita/vgx_compact_seed.h) is what the game
    uses; a VPK of thousands of loose files installs slowly and was the 3.0 release blocker B2."""
    import zipfile
    recs, probs = vpk_list(path)
    probs = list(probs) if recs is None else check(recs)
    if not compact: return probs   # (--loose: the old loose layout, the selftest's stub seed only)
    with zipfile.ZipFile(path) as z:
        names = [i.filename for i in z.infolist() if not i.is_dir()]
    loose = [n for n in names if re.search(r"shadercache/[0-9a-f]{8}/v3/[vf]/", n)]
    if len(names) > MAX_MEMBERS: probs.append(f"{len(names)} members (limit {MAX_MEMBERS}): the seed is not compact")
    if loose: probs.append(f"{len(loose)} loose shader files under v3/[vf]/ (first: {loose[0]}): the seed is not compact")
    if not any(n.endswith("/v3/seed.c182") for n in names): probs.append("no shadercache/<key>/v3/seed.c182 member: the compact seed is missing")
    return probs


def package(base, seed, dst, compact_seed=True):
    """the release VPK (see the usage): BASE without shadercache/, SEED's shadercache/ (the stage list tagged), then checkvpk. -> problems ([] = ok)"""
    import os, zipfile
    from pathlib import Path as P
    seedp = P(seed)
    members = []   # (name in the VPK, bytes)
    if seedp.is_file():
        with zipfile.ZipFile(seedp) as z:
            for i in z.infolist():
                if i.filename.startswith("shadercache/") and not i.is_dir(): members.append((i.filename, z.read(i)))
    else:
        root = seedp if seedp.name == "shadercache" else seedp / "shadercache"
        if not root.is_dir(): raise SystemExit(f"stagetags package: {seed}: no shadercache/ folder")
        for f in sorted(root.rglob("*")):
            if f.is_file() and not f.is_symlink(): members.append(("shadercache/" + f.relative_to(root).as_posix(), f.read_bytes()))
    if not any(n == "shadercache/vgx_stagewarm.bin" for n, _ in members): raise SystemExit(f"stagetags package: {seed} has no shadercache/vgx_stagewarm.bin")
    if compact_seed and any(n.endswith("/v3/seed.c182") for n, _ in members):
        print("package: the seed is already compact (v3/seed.c182): kept as it is")
    elif compact_seed:
        # (fixnow31) the release seed is one indexed file, not thousands of loose members; strict: a shader the pack cannot hold is an error, not left loose
        sys.path.insert(0, str(P(__file__).resolve().parents[1]))
        from c182_compact_seed import compact, MAX_ENTRIES, MAX_BODY
        before = len(members)
        try: packed = compact(dict(members), strict=True)
        except ValueError as e: raise SystemExit(f"stagetags package: {e}")
        members = list(packed.items())
        seedf = next(n for n, _ in members if n.endswith("/v3/seed.c182"))
        import struct as _st
        ent = _st.unpack_from("<8sIIQII", dict(members)[seedf])[2]; body = len(dict(members)[seedf])
        print(f"package: compact seed: {before} seed files -> {len(members)} members; {ent} of {MAX_ENTRIES} entries (headroom {MAX_ENTRIES - ent}), "
              f"{body:,} bytes (cap {MAX_BODY:,} on the body, headroom {MAX_BODY - body:,})")
    with zipfile.ZipFile(base) as zi, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
        kept = 0
        for info in zi.infolist():
            if info.filename.startswith("shadercache/"): continue
            zo.writestr(info, zi.read(info)); kept += 1
        for n, data in members:
            if n == "shadercache/vgx_stagewarm.bin":
                out, rep = legacy(parse(data, n)); data = encode(out)
                print(f"package: {n}: {rep}")
            info = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zo.writestr(info, data)
    probs = checkvpk(dst, compact=compact_seed)
    if probs: os.remove(dst)
    else: print(f"package: {dst}: {kept} members of {base}, {len(members)} of {seed}'s shadercache/; stage list tagged (stamp {stamp_of(vpk_list(dst)[0])})")
    return probs


def normalize(recs):
    own = {}   # (stage, vs, fs, draw) -> index into out of the kept own record
    cand = {}  # same, can-draw
    out = []
    drop = set()
    for st, lb, vs, fs in recs:
        key = (st, vs, fs, lb & DRAW, lb & MENUCLS)
        book = cand if lb & CANDRAW else own
        if key in book:
            i = book[key]
            ost, olb, ovs, ofs = out[i]
            if (olb & SCEN) != (lb & SCEN):
                olb &= ~SCEN  # (seen in two scenarios, or hinted and unhinted: any)
            olb |= lb & SLOW
            out[i] = (ost, olb, ovs, ofs)
            continue
        book[key] = len(out)
        out.append((st, lb, vs, fs))
    for k, i in cand.items():
        if k in own: drop.add(i)
    return [r for i, r in enumerate(out) if i not in drop]


def show(recs):
    per = {}
    st_ = stamp_of(recs)
    print("stamp: " + ("version %d, %d slow records, %d seed player records when written" % st_ if st_ else "NONE (not made by stagetags.py legacy)"))
    for st, lb, vs, fs in recs:
        if st == STAMP_KEY: continue
        p = per.setdefault(st, [0, 0, 0, 0, [0] * 8])
        p[0] += 1
        p[1] += bool(lb & SLOW)
        p[2] += bool(lb & SCEN)
        p[3] += bool(lb & CANDRAW)
        p[4][(lb & SCEN) >> SCEN_SHIFT] += 1
    for st in sorted(per):
        n, slow, hinted, cd, by = per[st]
        name = "PLYR" if st == PLAYER_KEY else "MENU" if st == MENU_KEY else "%08x" % st
        print(f"{name}\t{n} records\t{slow} slow\t{hinted} hinted {by[1:]}\t{cd} can-draw")
    print(f"total\t{len(recs)} records, {len(per)} stage keys")


def synth():
    """Observatory (4903d024) 697 records with its six at 648-651, 669, 671; Honeyhive (17bab031) 499 with its three at 497-499 (C181 test19, docs/c182-first-visit.md);
    a player list of 40 records sharing some programs; filler keys made from the index. Duplicate draws and programs of other stages are mixed in."""
    recs = []
    def filler(stage, i):
        return (stage, (0x16400 + (i % 5) * 0x800) & DRAW, 0x1000000000 + stage * 7 + (i % 211), 0x2000000000 + stage * 3 + i)
    obs = {648: 0, 649: 1, 650: 2, 651: 3, 669: 4, 671: 5}
    for i in range(1, 698):
        if i in obs:
            st, vs, fs, w = HISTORICAL[obs[i]]; recs.append((st, w, vs, fs))
        else:
            recs.append(filler(0x4903D024, i))
    for i in range(1, 500):
        if i >= 497:
            st, vs, fs, w = HISTORICAL[6 + i - 497]; recs.append((st, w, vs, fs))
        else:
            recs.append(filler(0x17BAB031, i))
    for i in range(40):
        recs.append((PLAYER_KEY, 0x16400 + (i % 3), 0x1000000000 + 0x4903D024 * 7 + (i % 211), 0x3000000000 + i))
    recs.append((0x4903D024, 0x16400, recs[10][2], recs[10][3]))  # (a duplicate draw)
    for k in (6, 7, 8):  # (Honeyhive's three under another stage key, as in test19's 3c2b6fbe: never a priority there)
        st, vs, fs, w = HISTORICAL[k]; recs.append((0x3C2B6FBE, w, vs, fs))
    for i, (v32, f32, w) in enumerate(PLY_SEED[:6]):  # (keys that end in seed entries, in other stages' records: the seed resolves to them)
        recs.append((0x17BAB031, w if i % 2 else 0x16C00, 0xABC0 << 32 | v32, (0xDEF0 + i) << 32 | f32))
    recs.append((PLAYER_KEY, PLY_SEED[1][2], (0xABC0 << 32) | PLY_SEED[1][0], (0xDEF1 << 32) | PLY_SEED[1][1]))  # (one seed pair already a player record)
    return recs


def selftest():
    r = synth()
    t, rep = legacy(r)
    slow = [i for i, x in enumerate(t) if x[1] & SLOW]
    assert slow == [647, 648, 649, 650, 668, 670, 697 + 496, 697 + 497, 697 + 498], (slow, rep)  # (exactly the nine: not the copy under another stage key)
    assert stamp_of(t) == (STAMP_VERSION, 9, 6) and t[-1][0] == STAMP_KEY and "5 PLYR records added at the end, 1 were there already" in rep, (stamp_of(t), rep)
    t2, rep2 = legacy(t)
    assert t2 == t and "0 tagged slow, 9 were tagged already" in rep2, rep2
    assert check(t) == [] and len(check(r)) == 2, (check(t), check(r))
    # the seed: each half resolves to the one key of the list that ends in it (whatever the record's draw word); the pair goes to the end as a PLYR record
    v32, f32, w = PLY_SEED[0]
    one = [(0x11, 0x400, (7 << 32) | v32, 5), (0x12, 0x800, 6, (9 << 32) | f32), (PLAYER_KEY, 0x400, 1, 2)]
    t3, rep3 = legacy(one)
    assert t3[3] == (PLAYER_KEY, w, (7 << 32) | v32, (9 << 32) | f32) and "1 of 11 entries resolve" in rep3 and stamp_of(t3)[2] == 1, (t3, rep3)
    t4, rep4 = legacy(one + [(0x13, w, (8 << 32) | v32, 3)])
    assert not any(x[0] == PLAYER_KEY and x[2] & 0xFFFFFFFF == v32 for x in t4) and "(2 vs, 1 fs keys)" in rep4, rep4
    t5, rep5 = legacy(one + [(PLAYER_KEY, w, (7 << 32) | v32, (9 << 32) | f32)])
    assert "0 PLYR records added at the end, 1 were there already" in rep5, rep5
    # normalize
    s2, s3 = 2 << SCEN_SHIFT, 3 << SCEN_SHIFT
    n = normalize([(1, 0x400 | s2, 5, 6), (1, 0x800, 5, 6), (1, 0x400 | s3 | SLOW, 5, 6), (1, 0x800 | CANDRAW, 5, 6), (1, 0xC00 | CANDRAW | s2, 5, 6),
                   (1, 0xC00 | CANDRAW | s3, 5, 6), (2, 0x400 | s2, 5, 6), (1, 0x1000 | s2, 5, 6)])
    assert n == [(1, 0x400 | SLOW, 5, 6), (1, 0x800, 5, 6), (1, 0xC00 | CANDRAW, 5, 6), (2, 0x400 | s2, 5, 6), (1, 0x1000 | s2, 5, 6)], n
    # (fix31) package / checkvpk: the release VPK's stage list is tagged; a VPK with an untagged list, or none, fails the hard check
    import tempfile, zipfile, os
    with tempfile.TemporaryDirectory() as td:
        base, seedd, out, bad = (os.path.join(td, n) for n in ("base.vpk", "seed", "out.vpk", "bad.vpk"))
        with zipfile.ZipFile(base, "w") as z:
            z.writestr("eboot.bin", b"SCE"); z.writestr("sce_sys/param.sfo", b"SFO"); z.writestr("shadercache/old.bin", b"stale")
        os.makedirs(os.path.join(seedd, "shadercache", "b4d4c2a9", "v3", "v", "AB"))
        write(os.path.join(seedd, "shadercache", "vgx_stagewarm.bin"), synth())
        open(os.path.join(seedd, "shadercache", "b4d4c2a9", "v3", "v", "AB", "AB12.gxp"), "wb").write(b"gxp")
        assert package(base, seedd, out, compact_seed=False) == [] and checkvpk(out, compact=False) == []
        with zipfile.ZipFile(out) as z:
            names = sorted(z.namelist())
            assert names == ["eboot.bin", "sce_sys/param.sfo", "shadercache/b4d4c2a9/v3/v/AB/AB12.gxp", "shadercache/vgx_stagewarm.bin"], names
            assert stamp_of(parse(z.read("shadercache/vgx_stagewarm.bin")))
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("eboot.bin", b"SCE"); z.writestr("shadercache/vgx_stagewarm.bin", encode(synth()))
        assert any("no stamp" in x for x in checkvpk(bad))
        with zipfile.ZipFile(bad, "w") as z: z.writestr("eboot.bin", b"SCE")
        assert any("0 shadercache/vgx_stagewarm.bin" in x for x in checkvpk(bad))
    print("stagetags selftest: legacy tags (the nine, stage-exact, idempotent, stamped), seed as PLYR data (per-key resolution, ambiguity refused), check, normalize,"
          " package / checkvpk (fix31) PASS")


def main(argv):
    if len(argv) < 2: raise SystemExit(__doc__)
    cmd = argv[1]
    if cmd == "selftest": selftest(); return
    if cmd == "show": show(read(argv[2])); return
    if cmd == "synth": write(argv[2], synth()); print(f"synth: {len(synth())} records -> {argv[2]}"); return
    if cmd == "legacy":
        extra = []
        args = argv[2:]
        while "--ply" in args:
            i = args.index("--ply"); extra += read(args[i + 1]); del args[i:i + 2]
        out, rep = legacy(read(args[0]), extra)
        write(args[1], out)
        print(f"legacy: {args[0]} -> {args[1]}: {rep}")
        return
    if cmd == "check":
        probs = check(read(argv[2]))
        for p in probs: print(f"check: {argv[2]}: {p}")
        if probs: raise SystemExit(1)
        print(f"check: {argv[2]}: tagged (stamp {stamp_of(read(argv[2]))})")
        return
    if cmd == "vpk":
        vpk(argv[2], argv[3]); return
    if cmd == "checkvpk":
        probs = checkvpk(argv[2], compact="--loose" not in argv[3:])
        for p in probs: print(f"checkvpk: {argv[2]}: {p}")
        if probs: raise SystemExit(1)
        print(f"checkvpk: {argv[2]}: the packaged stage list is tagged (stamp {stamp_of(vpk_list(argv[2])[0])})")
        return
    if cmd == "package":
        probs = package(argv[2], argv[3], argv[4], compact_seed="--loose" not in argv[5:])
        for p in probs: print(f"package: FAILED: {p}")
        if probs: raise SystemExit(1)
        return
    if cmd == "normalize":
        r = read(argv[2]); out = normalize(r); write(argv[3], out)
        print(f"normalize: {len(r)} -> {len(out)} records")
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)

#!/usr/bin/env python3
"""misskeys.py: the offline side of GXV_PACK_MISSLOG (packrefill25; platform/vgx/vita/vgx_pack_misslog.h). Run it with python3 -I.

The game logs ONE line per distinct shader text the packs did not answer, and a summary every 60 s:
    pack-miss: f/3A/3A1B2C3D4E5F6071.gxp stage=099e9a42 play=1 t=123456 n=17        (the name is the pack record's / vitaGL cache file's name)
    pack: hits 1203 misses 4 compiled-in-play 0 (window 60 s: compiled 4 in all, user pack hits 0; since start: hits .. misses .. (N distinct keys, O past the table) compiled .., compiled-in-play ..)
    pack: a shader compile ran in play: stage=099e9a42 412 ms (n=1 in play)

  misskeys.py keys   LOG [LOG ...] [--out names.txt] [--tsv detail.tsv]
        the distinct keys of the logs: names.txt = one name per line, sorted (refill_pack.py --expect / --misslog take the logs themselves; this file is for
        compare / diff). Prints counts (vertex / fragment, first seen in play, by stage) and the last "pack:" summary of each log.
  misskeys.py check  LOG [LOG ...] [--allow names.txt] [--max-misses N]
        VERIFICATION: exit 0 only when the logs show no distinct pack miss beyond --allow / --max-misses (default 0), no "pack: ... compiled-in-play" above 0,
        no "a shader compile ran in play" line, and no "did not compile / link failed / compile failed / FATAL" line. Also fails when a log has no
        "pack:" summary or no "GXV_PACK_MISSLOG=1" banner (the run did not log: its silence proves nothing).
  misskeys.py compare PACK names.txt
        every name of names.txt is a record of the VGXRAM01 pack file (stages.pack); prints the missing ones; exit 0 when none.
  misskeys.py check ... [--profile-word HEX16] [--fixed-point]     (seedgate31 A2)
        --profile-word: every log must carry the game's "profile: word=<16 hex>" line (platform/vgx/src/vgx_shadergen.h VGX_TEXT_SWITCHES) and it must equal HEX16, the
        release tree's word (refill_pack.py prints it; --gate-only keeps it in REFILL-REPORT.txt). A log of another profile proves nothing about the release: FAIL.
        --fixed-point: also every "cover:" line must show compiles=0, new_recs=0 and new_candraw=0 (the second walk of A.5.1: nothing learned, nothing compiled).
        new_recs / new_candraw are the own and can-draw records the visit LEARNED (fix31: the start-up loads are not counted; visit totals, so every line of a
        visit is checked, not only a stage's last one). A cover: line without new_candraw is from a build before fix31, whose new_recs counted the start-up
        loads and no can-draw record: it proves nothing about a fixed point and fails the check.
        A "cap-full:" line (a stage-list cap reached; it used to be silent) always fails the check.
  misskeys.py profile LOG [LOG ...]
        the profile word(s) of the logs and, when two differ, the switches that differ. Exit 3 when the logs do not all have one word.
  misskeys.py cover LOG [LOG ...] [--tsv out.tsv]
        the census table: the last "cover:" line of every stage of every log (links in play, compiles in play, new records, uploads, targets, hold).
Exit codes: 0 ok, 1 usage / input error, 3 check failed.
"""
import argparse, re, struct, sys
from pathlib import Path

MISS = re.compile(r"pack-miss: (?P<name>[vf]/[0-9A-F]{2}/[0-9A-F]{1,16}\.gxp)(?P<rest>.*)$")
SUMMARY = re.compile(r"pack: hits (\d+) misses (\d+) compiled-in-play (\d+) \(window (\d+) s: compiled (\d+) in all, user pack hits (\d+); since start: hits (\d+) misses (\d+) "
                     r"\((\d+) distinct keys, (\d+) past the table\) compiled (\d+), compiled-in-play (\d+)\)")
INPLAY = re.compile(r"pack: a shader compile ran in play: stage=([0-9a-f]{8}) (\d+) ms \(n=(\d+) in play\)")
BANNER = "GXV_PACK_MISSLOG=1"
FAIL = re.compile(r"did not compile|link failed|compile failed|FATAL")
# seedgate31 A0: the game's profile stamp, the census line and the cap line (log only; see vgx_draw.cpp LogProfileStamp, vgx_backend_gl.cpp FirstUseSummary)
PROFILE = re.compile(r"profile: word=([0-9a-f]{16}) gen=(\d+) switches=(\d+)")
PROFILE_SW = re.compile(r"profile-sw:(.*)$")
COVER = re.compile(r"cover: (stage=[0-9a-f]{8} .*)$")
CAPFULL = re.compile(r"cap-full: (.*)$")

def parse_kv(text):
    return dict(item.split("=", 1) for item in text.split() if "=" in item)

def canonical(name):
    m = re.fullmatch(r"([vf])/([0-9A-F]{2})/([0-9A-F]{1,16})\.gxp", name)
    if not m: return False
    d = m.group(3)
    return not (len(d) > 1 and d[0] == "0") and (int(d, 16) >> 56) == int(m.group(2), 16)

class LogInfo:
    def __init__(self, path):
        self.path = path; self.keys = {}; self.summaries = []; self.inplay = []; self.banner = False; self.failures = []; self.lines = 0
        self.profiles = []; self.switches = {}; self.covers = []; self.capfull = []

def read_log(path):
    info = LogInfo(str(path))
    with open(path, errors="replace") as f:
        for line in f:
            info.lines += 1
            if BANNER in line and "pack:" in line: info.banner = True
            m = PROFILE.search(line)
            if m: info.profiles.append((m.group(1), int(m.group(2)), int(m.group(3)))); continue
            m = PROFILE_SW.search(line)
            if m: info.switches.update(parse_kv(m.group(1))); continue
            m = COVER.search(line)
            if m: info.covers.append(parse_kv(m.group(1))); continue
            m = CAPFULL.search(line)
            if m: info.capfull.append(m.group(1).strip()[:200]); continue
            m = MISS.search(line)
            if m and canonical(m.group("name")):
                rest = m.group("rest")
                st = re.search(r"stage=([0-9a-f]{8})", rest); pl = re.search(r"play=([01])", rest); t = re.search(r"t=(\d+)", rest)
                info.keys.setdefault(m.group("name"), {"stage": st.group(1) if st else "", "play": int(pl.group(1)) if pl else 0, "t": int(t.group(1)) if t else 0})
                continue
            m = SUMMARY.search(line)
            if m: info.summaries.append(tuple(int(x) for x in m.groups())); continue
            m = INPLAY.search(line)
            if m: info.inplay.append((m.group(1), int(m.group(2)), int(m.group(3)))); continue
            if FAIL.search(line): info.failures.append(line.strip()[:200])
    return info

def read_logs(paths):
    """-> (merged {name: {...first sighting}}, [LogInfo]); the first log that has a key wins its detail"""
    infos = [read_log(p) for p in paths]
    merged = {}
    for i in infos:
        for k, v in i.keys.items(): merged.setdefault(k, dict(v, log=i.path))
    return merged, infos

def log_profile(info):
    """-> (word or None, problem or None): the one profile word of a log"""
    words = sorted({w for w, _, _ in info.profiles})
    if not words: return None, f"{info.path}: no 'profile: word=' line (a run of a build before the profile stamp, or the log is cut): its profile is unknown"
    if len(words) > 1: return None, f"{info.path}: {len(words)} different profile words in one log ({', '.join(words)})"
    return words[0], None

def switch_diff(a, b):
    """the switches that differ between two {name: value} dicts, as text lines"""
    return [f"{k}: {a.get(k, '?')} / {b.get(k, '?')}" for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]

def last_covers(info):
    """{stage: last cover line}; a 'left' / 'exit' line wins over a later 'periodic' one of the same visit only by order, so the plain last line counts"""
    out = {}
    for c in info.covers: out[c.get("stage", "?")] = c
    return out

def cmd_profile(a):
    infos = [read_log(p) for p in a.logs]
    words = {}
    for i in infos:
        w, problem = log_profile(i)
        print(f"{i.path}: " + (f"profile word {w}, {len(i.switches)} switches logged" if w else problem))
        if w: words.setdefault(w, i)
    if len(words) > 1:
        ws = sorted(words)
        print(f"DIFFERENT PROFILES: {', '.join(ws)}")
        for x in ws[1:]:
            for d in switch_diff(words[ws[0]].switches, words[x].switches): print(f"  {ws[0]} / {x}: {d}")
    ok = len(words) == 1 and all(log_profile(i)[0] for i in infos)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 3

COVER_FIELDS = ("play_s", "links", "link_ms", "link_worst_ms", "compiles", "compile_ms", "new_recs", "new_candraw", "uploads", "targets", "hold_ms")
def cmd_cover(a):
    rows = []
    for p in a.logs:
        i = read_log(p); w, _ = log_profile(i)
        for st, c in sorted(last_covers(i).items()): rows.append((p, w or "?", st, c))
    print("log\tprofile\tstage\twhy\t" + "\t".join(COVER_FIELDS))
    lines = [f"{p}\t{w}\t{st}\t{c.get('why', '')}\t" + "\t".join(c.get(f, "") for f in COVER_FIELDS) for p, w, st, c in rows]
    for l in lines: print(l)
    if a.tsv: Path(a.tsv).write_text("log\tprofile\tstage\twhy\t" + "\t".join(COVER_FIELDS) + "\n" + "".join(l + "\n" for l in lines))
    return 0 if rows else 3

def cmd_keys(a):
    merged, infos = read_logs(a.logs)
    nv = sum(1 for k in merged if k[0] == "v"); nf = len(merged) - nv
    inplay = [k for k, v in merged.items() if v["play"]]
    print(f"{len(merged)} distinct keys from {len(infos)} log(s): {nv} vertex, {nf} fragment; first missed in play: {len(inplay)}")
    by = {}
    for v in merged.values(): by[v["stage"]] = by.get(v["stage"], 0) + 1
    for s, n in sorted(by.items(), key=lambda x: -x[1])[:12]: print(f"  first missed under stage {s or '?'}: {n}")
    for i in infos:
        last = i.summaries[-1] if i.summaries else None
        pw = log_profile(i)[0]
        print(f"{i.path}: {len(i.keys)} keys, banner {'yes' if i.banner else 'NO'}, profile {pw or 'NONE'}, summaries {len(i.summaries)}"
              + (f", last: hits {last[6]} misses {last[7]} ({last[8]} distinct) compiled {last[10]} compiled-in-play {last[11]}" if last else ""))
    if a.out: Path(a.out).write_text("".join(f"{k}\n" for k in sorted(merged)))
    if a.tsv: Path(a.tsv).write_text("".join(f"{k}\t{v['stage']}\t{v['play']}\t{v['t']}\t{v['log']}\n" for k, v in sorted(merged.items())))
    return 0 if merged else 3

def fixed_point_problems(info):
    """(seedgate31, fix31) --fixed-point: every cover: line (each a visit's totals so far) shows nothing compiled in play and nothing learned, own or can-draw"""
    bad = []
    if not info.covers: return [f"{info.path}: no 'cover:' line (--fixed-point needs the run's census lines)"]
    old = sum(1 for c in info.covers if "new_candraw" not in c)
    if old: bad.append(f"{info.path}: {old} cover: line(s) without new_candraw (a build before fix31: its new_recs counts the start-up loads and no can-draw record)")
    said = set()
    for c in info.covers:
        st = c.get("stage", "?")
        for f, what in (("compiles", "in play"), ("new_recs", "own records learned"), ("new_candraw", "can-draw records learned")):
            v = c.get(f, "0")
            if v != "0" and (st, f) not in said:
                said.add((st, f)); bad.append(f"{info.path}: stage {st}: {f}={v} {what} (fixed point needs 0; why={c.get('why', '?')})")
    return bad

def cmd_check(a):
    merged, infos = read_logs(a.logs)
    allow = set()
    if a.allow: allow = {l.split()[0] for l in Path(a.allow).read_text().splitlines() if l.strip() and not l.startswith("#")}
    bad = []
    for i in infos:
        if not i.banner: bad.append(f"{i.path}: no 'GXV_PACK_MISSLOG=1' banner (GXV_PACK_MISSLOG was not on: the log proves nothing)")
        if not i.summaries: bad.append(f"{i.path}: no 'pack: hits .. misses .. compiled-in-play ..' summary (run shorter than a minute, or the log is cut)")
        for s in i.summaries:
            if s[2]: bad.append(f"{i.path}: a window shows compiled-in-play {s[2]}")
        for st, ms, n in i.inplay: bad.append(f"{i.path}: shader compile in play: stage {st}, {ms} ms")
        for f in i.failures: bad.append(f"{i.path}: {f}")
        for c in sorted(set(i.capfull)): bad.append(f"{i.path}: cap-full: {c}")
        if a.profile_word:
            w, problem = log_profile(i)
            if problem: bad.append(problem)
            elif w != a.profile_word.lower(): bad.append(f"{i.path}: profile word {w} is not the release tree's {a.profile_word.lower()} (another set of shader text switches: this run's keys and texts are not the release's)")
        if a.fixed_point: bad += fixed_point_problems(i)
    extra = sorted(k for k in merged if k not in allow)
    if len(extra) > a.max_misses: bad.append(f"{len(extra)} distinct pack misses (allowed {a.max_misses}): " + ", ".join(extra[:8]) + (" ..." if len(extra) > 8 else ""))
    hits = sum(i.summaries[-1][6] for i in infos if i.summaries)
    print(f"check: {len(infos)} log(s), {len(merged)} distinct misses ({len(extra)} not allowed), pack hits so far {hits}")
    for b in sorted(set(bad)): print("  FAIL:", b)
    print("RESULT:", "PASS" if not bad else "FAIL")
    return 0 if not bad else 3

def read_pack_names(path):
    d = Path(path).read_bytes()
    if len(d) < 40 or d[:8] != b"VGXRAM01": raise SystemExit(f"misskeys: {path} is not a VGXRAM01 pack")
    n = struct.unpack_from("<I", d, 12)[0]
    abi = struct.unpack_from("<Q", d, 24)[0]
    names = set()
    for i in range(n):
        st, ln, h, off = struct.unpack_from("<IIQQ", d, 40 + 24 * i)
        names.add(f"{'v' if st == 1 else 'f'}/{h >> 56:02X}/{h:X}.gxp")
    return names, n, abi, len(d)

def cmd_compare(a):
    names, n, abi, size = read_pack_names(a.pack)
    want = [l.split()[0] for l in Path(a.names).read_text().splitlines() if l.strip() and not l.startswith("#")]
    missing = sorted(set(w for w in want if w not in names))
    print(f"pack {a.pack}: {n} records, {size} bytes, ABI word {abi:016x}; {len(set(want))} wanted names, {len(missing)} missing")
    for m in missing[:20]: print("  missing:", m)
    if len(missing) > 20: print(f"  ... {len(missing) - 20} more")
    print("RESULT:", "PASS" if not missing else "FAIL")
    return 0 if not missing else 3

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keys"); k.add_argument("logs", nargs="+"); k.add_argument("--out"); k.add_argument("--tsv")
    c = sub.add_parser("check"); c.add_argument("logs", nargs="+"); c.add_argument("--allow"); c.add_argument("--max-misses", type=int, default=0)
    c.add_argument("--profile-word"); c.add_argument("--fixed-point", action="store_true")
    pr = sub.add_parser("profile"); pr.add_argument("logs", nargs="+")
    cv = sub.add_parser("cover"); cv.add_argument("logs", nargs="+"); cv.add_argument("--tsv")
    p = sub.add_parser("compare"); p.add_argument("pack"); p.add_argument("names")
    a = ap.parse_args()
    for lg in getattr(a, "logs", []):
        if not Path(lg).is_file(): raise SystemExit(f"misskeys: no such log {lg}")
    return {"keys": cmd_keys, "check": cmd_check, "compare": cmd_compare, "profile": cmd_profile, "cover": cmd_cover}[a.cmd](a)

if __name__ == "__main__":
    sys.exit(main())

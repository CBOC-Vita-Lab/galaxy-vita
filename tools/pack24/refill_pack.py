#!/usr/bin/env python3
"""refill_pack.py: merge a console's pulled shader cache with the package's compiled-shader seed and rebuild the package's shadercache/
(pack23, extended by pack24 for GXV_TEVIN_CANON=2 and the clean refill, Galaxy Vita 2.3). Run it with python3 -I. Nothing in the inputs is
changed; everything is written under --out (which must not exist).

  python3 -I refill_pack.py --package <2.x .vpk | its shadercache/ folder> --pulled <pull_tree's shadercache.tgz | a folder> [--pulled ...]
        --out <new folder> --source <the tree the eboot was built from> [--identity TEXT | --log <log.txt with "compiled-shader cache: ... (key of: ...)">]
        [--expect fognames-bind2.txt ...] [--list card|package] [--stagewarm card|union|package] [--seed-policy needed|all|none]
        [--tour-stages FILE] [--prefer package|card]

seedgate31 (A2) changes: the merge is a GATE. Nothing is built from a harvest of another shader profile, and a seed that is not closed fails.
  --harvest <folder> (repeatable)   an EMULATOR harvest (tools/seedwalk: the card delta of a leg, with its log). Its lists and records are used like a pull's; its
        compiled files only when proven: every text that exists both in the console-made inputs (package seed, --pulled) and in the harvest must be byte-identical
        (COMPILER-IDENTITY.txt; at least --min-overlap texts, default 25). Any difference, or too few to prove it, and the harvest's files are left out and the run
        fails (the emulator then supplies lists only and one console compile job the files); --harvest-files lists-only chooses that on purpose.
  profile word: the game logs "profile: word=<16 hex>" (platform/vgx/src/vgx_shadergen.h VGX_TEXT_SWITCHES, the one list of every switch that changes a key or text;
        TEXT_SWITCHES below is read from it). The word of the release tree is made from --source's header and vita/vgx-game/entry.cpp defaults (--release-word HEX16
        overrides it, e.g. from a release run's log). Every log found in a --harvest folder or given as --profile-log must carry a word and it must equal the
        release's, else the run is REFUSED (exit 4) before anything is written: retail is GXV_VSOUT=1, a GXV_VSOUT=0 harvest is dead weight that passes every format
        check. Logs of --pulled folders and --misslog are checked when they have a word (--require-profile: they must).
  closure gate: every stage record (menu and player records included) must have both key recipes in the final key list (vgx_shaderlist.bin) and both compiled
        files in the merged folder; open records fail the run (CLOSURE-REPORT.txt, MISSING-closure.txt; --closure report only reports). Every pack-miss key of
        --misslog already had to have a file.
  caps (all loud, all failures): own records (the stages', menu and player records) 2,048 a stage and 32,768 in all (the game's memory); can-draw records apart,
        1,024 a stage and 16,384 in all (kCanDrawStageMax / kCanDrawMemMax: the game keeps them in a tier of their own); 65,536 records in the file; 4,096 player
        records, 8,192 RAM-pack records / 8 MiB, and the indexed compact seed (8,192 entries / 8 MiB, 4,096 bytes a file: tools/c182_compact_seed.py). --pack-overflow
        usage puts the most widely used texts that fit the limits (stages that need them) into stages.pack when there are more; the rest stay in the indexed seed.
  stage-list merge (fix31): the inputs' records are merged on the DRAW, not on the record word: stage, both key hashes, the draw bits and the menu class
        (tools/seedrun/stagetags.py normalize). A draw recorded under two scenario hints, or hinted and unhinted, is one unhinted record; slow is kept if any copy was
        slow; a can-draw record of a draw the stage's own records hold is dropped. So a union of many legs holds each draw once, whatever hints the legs gave it.
        Each card list folder is read as the game reads the card: vgx_stagewarm.bin, its journal vgx_stagewarm.bin.jnl (seedrun31 A3: what a leg learned in its last
        seconds) and the player file vgx_stagewarm.bin.ply (for both: a complete .tmp first, as the game does).
  --gate-only: no new input: gate the package's own seed (profile of --profile-log, closure, caps) and write the closure report (a release artifact).
  prune by evidence (harvprep31, --prune fill|all|off, default fill): a harvest leg (GXV_SEEDWALK=1) keeps up to 8,192 own records a stage (the game's
        kStageMaxRecsHarvest), so a stage whose package list was full already (test19's menu list: 2,048) still records what the leg draws; each leg also leaves its
        draw evidence (vgx_stagewarm.bin.drawn, 'VGXSTD01': every distinct draw it made, held records included). After the merge, a stage over the release's 2,048
        own records is pruned back by that evidence: every record some leg drew is kept (ranked by how many legs drew it, PRUNE-RANK.tsv), the records no leg drew
        are dropped, the latest in list order first, until the stage fits (fill) or all of them (all); list order is kept. Every dropped record is logged
        (PRUNED-records.txt and a line per stage); a stage whose drawn records alone are over the cap fails the run (CAP:), and so does an over-full stage when no
        harvest folder has evidence. The release runtime's caps do not change.

pack24 changes (read the pack24 REPORT):
  - tools/listnames.cpp (next to this file) is the mode-2 version: it converts every list key the way the game does at load
    (CanonListFsKey), so "expected from the list" names the texts a GXV_TEVIN_CANON=2 build draws. The tool binary is rebuilt whenever
    listnames.cpp, vgx_shadergen.cpp or vgx_shadergen.h change (its name carries their hash): no stale binary can be reused.
  - The game's GXV_ defaults are read from the source tree's entry.cpp; the run refuses if GXV_TEVIN_CANON differs between the tree and the
    env the card run used (the log's "GXV_TEVIN_CANON" is not printed, so the check is the tree's own value, printed in the report).
  - --seed-policy needed (default): a package seed file is kept only if the current generator names it (list or card list). The 2.2 seed is
    mostly stale under the 2.3 defaults (831 of its 2,788 files are drawn), and ALL of it would take the pack past 4,096 records.
    all = pack23's behaviour, none = the card's files alone (the clean refill: the card ran with GXV_VITA_SEED=off and compiled everything).
  - --list card / --stagewarm card are the defaults (the clean refill leaves the card with the whole list and the tour's stage lists).
    The player list (PLYR records, the card's vgx_stagewarm.bin.ply) is merged into vgx_stagewarm.bin.
  - --tour-stages: a text file with one galaxy / stage name per line (default: the names in tour_legs.txt): each must have records in the
    final stage list (FNV-1a of the name is the stage key).

packrefill25 (GXV_PACK_MISSLOG): --misslog LOG [--misslog ...] takes the "pack-miss:" lines of the collect tour's logs (tools/pack25/misskeys.py): every logged key must
  have a compiled file in the merged set (MISSING-misslog.txt, check fails otherwise); --only-logged then packs exactly the logged keys (the files nobody asked for in
  the final-switch tour go to UNASKED-FILES.txt: stale card leftovers, other switches' texts) and takes nothing from the old package pack or seed.

Output (drop-in for the VPK's shadercache/ folder; docs/BUILDING.md "shadercache/"):
  <out>/shadercache/<key>/v3/{v,f}/<HH>/<HASH>.gxp   package seed + every console-compiled file it lacked
  <out>/shadercache/<key>/stages.pack                 the RAM pack rebuilt from exactly that folder (tools/vita_shader_pack.cpp; same identity),
                                                      checked with tests/vita_shader_pack_roundtrip.cpp. Since 571f42d (smooth23) the game
                                                      loads it from the package (GXV_SHADERPACK=1, GXV_SHADERPACK_SEED=1): it must always be
                                                      rebuilt with the seed, or it goes stale.
  <out>/shadercache/vgx_shaderlist.bin                the package's warm-up list (default) or the card's (--list card)
  <out>/shadercache/vgx_stagewarm.bin                 union of the package's and the card's per-galaxy lists (package records first)
  <out>/REFILL-REPORT.txt, MISSING-*.txt, SHA256SUMS
Exit status: 0 = all checks passed; 1 = usage / input error; 3 = built, but a check failed (see the report: missing expected names, pack
over its limits, roundtrip failure).
The checks:
  - every name of each --expect file (e.g. gfx23's fognames-bind2.txt: the 210 fog texts of GXV_FOG_BIND=2) is in the merged folder;
  - with --source: the names the source tree's shader generator makes for every key of the final warm-up list (and of the card's list),
    with the game's entry.cpp defaults (tools/listnames.cpp), are all in the merged folder ("expected from the list");
  - stages.pack: identity -> folder key (FNV-1a), records == .gxp files, limits (8 MiB / 8192 records), roundtrip exact bytes.
  - every input file is checked as ShaderPack::StageImage checks a record (a copy cut short is ignored and listed).
--old-pack list (default, needs --source): list texts that neither the loose seed nor the card has are taken from the package's old
  stages.pack when it holds them (2.2: 223 of the 971 missing ones). The card's and the seed's files always win.
The tools (vita_shader_pack, the roundtrip test, tools/listnames.cpp) are built fresh from --tools-src / --source in <out>/_work.
"""
import argparse, hashlib, io, os, re, shutil, struct, subprocess, sys, tarfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent / "pack25"), str(HERE.parent), str(HERE.parent / "seedrun")]
import misskeys              # (profile / cover line parsing, shared with the verification tool)
import stagetags             # (seedrun31b: every stage list written here carries the first-visit tags and the old player seed as data)
import c182_compact_seed     # (the compact seed's limits)
NAME = re.compile(r"^([vf])/([0-9A-F]{2})/([0-9A-F]{1,16})\.gxp$")
MAX_PACK_BYTES, MAX_PACK_ENTRIES, MAX_RECORD = 8 << 20, 8192, 256 << 10   # (pack25: = ShaderPack::kMaxBytes / kMaxEntries)
PACK_HEADER, PACK_ENTRY = 40, 24
MENU_KEY = 0x4D454E55
PLAYER_KEY = 0x504C5952
STAGE_MAX_RECS, STAGE_FILE_MAX = 2048, 65536    # (vgx_backend_gl.cpp kStageMaxRecs, kStageFileMax)
STAGE_MEM_MAX, PLAYER_MAX = 32768, 4096          # (kStageMemMax: all stages in memory; the player list file)
CANDRAW_STAGE_MAX, CANDRAW_MEM_MAX = 1024, 16384 # (kCanDrawStageMax, kCanDrawMemMax: the can-draw tier, counted apart from the own records)
JOURNAL_MAX = 8192                               # (kJnlMaxRecs: the stage journal, 'VGXSTJ01')
STAGE_MAX_RECS_HARVEST, DRAWN_MAX = 8192, 65536  # (harvprep31: kStageMaxRecsHarvest of a GXV_SEEDWALK=1 run; kDrawnMaxRecs of its draw evidence, 'VGXSTD01')
report = []

# ---- the profile word (seedgate31 A0/A2): the switch list is the game's own -------------------------------------------------------------
SHADERGEN_H = "platform/vgx/src/vgx_shadergen.h"
def load_text_switches(tree):
    """[(name, default)] parsed from VGX_TEXT_SWITCHES in <tree>/platform/vgx/src/vgx_shadergen.h. The only source of the list: nothing is copied here."""
    text = (Path(tree) / SHADERGEN_H).read_text()
    start = text.find("#define VGX_TEXT_SWITCHES(X)")
    if start < 0: raise SystemExit(f"refill: {tree}/{SHADERGEN_H} has no VGX_TEXT_SWITCHES list (a tree older than the profile stamp)")
    block, lines = [], text[start:].split("\n")
    for ln in lines:
        block.append(ln)
        if not ln.rstrip().endswith("\\"): break
    items = re.findall(r'X\("([A-Z0-9_]+)",\s*"([^"]*)"\)', "\n".join(block))
    if not items: raise SystemExit(f"refill: VGX_TEXT_SWITCHES in {tree}/{SHADERGEN_H} holds no X(\"NAME\", \"default\") rows")
    return items

def gen_version(tree):
    return int(re.search(r"kShaderGenVersion = (\d+)", (Path(tree) / SHADERGEN_H).read_text()).group(1))

def load_opt_switches(tree):
    """[(name, default)] of VGX_TEXT_SWITCHES_OPT: they join the word only when not at their default"""
    text = (Path(tree) / SHADERGEN_H).read_text()
    start = text.find("#define VGX_TEXT_SWITCHES_OPT(X)")
    if start < 0: return []
    block = []
    for ln in text[start:].split("\n"):
        block.append(ln)
        if not ln.rstrip().endswith("\\"): break
    return re.findall(r'X\("([A-Z0-9_]+)",\s*"([^"]*)"\)', "\n".join(block))

def profile_word(items, values, gen, opt=None):
    """FNV-1a 64 over 'NAME=value\\n' per switch in list order, then 'gen=N\\n' (vgx::ProfileWord). values: {name: text}; unset or empty = the list's default"""
    h = 14695981039346656037
    text = "".join(f"{n}={values.get(n) or d}\n" for n, d in items)
    text += "".join(f"{n}={values[n]}\n" for n, d in (opt or []) if values.get(n) and values[n] != d) + f"gen={gen}\n"
    for c in text.encode(): h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h

def release_profile(tree):
    """the profile of the release the tree builds: (word, {name: value}, items, gen). The game's entry.cpp setenv(.., 0) defaults over the list's defaults."""
    tree = Path(tree)
    items, gen = load_text_switches(tree), gen_version(tree)
    env, _ = entry_defaults(tree)
    opt = load_opt_switches(tree)
    values = {n: (env.get(n) or d) for n, d in items + opt}
    return profile_word(items, values, gen, opt), values, items, gen

try: TEXT_SWITCHES = tuple(n for n, _ in load_text_switches(HERE.parent.parent))
except (SystemExit, OSError): TEXT_SWITCHES = ()   # (run from a copy without the tree: --source's own header is read by release_profile)

def scan_profile(path):
    """-> (set of words, {switch: value}) of one log; lines that are not the stamp are skipped"""
    words, sw = set(), {}
    with open(path, errors="replace") as f:
        for line in f:
            if "profile" not in line: continue
            m = misskeys.PROFILE.search(line)
            if m: words.add(m.group(1)); continue
            m = misskeys.PROFILE_SW.search(line)
            if m: sw.update(misskeys.parse_kv(m.group(1)))
    return words, sw

def logs_in(folder):
    """text logs of a harvest folder (any depth); the card's own files (.bin .gxp .pack .ply) are not read"""
    out = []
    for p in sorted(Path(folder).rglob("*")):
        if p.is_file() and p.suffix.lower() in (".log", ".txt", ".out", ".err") and p.stat().st_size < (1 << 31): out.append(p)
    return out

def profile_problems(label, logs, release_word, release_values, require):
    """-> list of refusal texts for one source (a harvest folder, a --pulled folder, --misslog logs). Each log with a word must equal the release's."""
    probs, found = [], 0
    for lg in logs:
        words, sw = scan_profile(lg)
        if not words: continue
        found += 1
        for w in sorted(words):
            if w == release_word: continue
            diff = [f"    {k}: harvest {sw.get(k, '0' if sw else '?')} / release {release_values.get(k, '?')}"
                    for k in sorted(release_values) if sw and sw.get(k, "0") != release_values[k]]
            probs.append(f"{label}: {lg} logged profile word {w}; the release tree's is {release_word}" + ("\n" + "\n".join(diff) if diff else "\n    (no switch line to compare)"))
    if require and not found: probs.append(f"{label}: no log with a 'profile: word=' line ({len(logs)} log file(s) looked at): its profile is unknown")
    return probs

def say(line=""):
    print(line); report.append(line)

def fnv1a(text):
    h = 2166136261
    for c in text.encode(): h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return "%08x" % h

def canonical(rel):
    """vita_shader_pack's ParsePath rules: <v|f>/<HH>/<HASH>.gxp, upper-case hex, no leading zero, HH = top byte of the hash"""
    m = NAME.match(rel)
    if not m: return False
    digits = m.group(3)
    if len(digits) > 1 and digits[0] == "0": return False
    return (int(digits, 16) >> 56) == int(m.group(2), 16)

# ---- inputs ------------------------------------------------------------------------------------------------------------------------
def safe_extract_tgz(path, dest):
    with tarfile.open(path, "r:*") as t:
        for m in t.getmembers():
            n = m.name
            if n.startswith("/") or ".." in Path(n).parts or not (m.isfile() or m.isdir()):
                raise SystemExit(f"refill: refusing archive member {n!r} in {path}")
        for m in t.getmembers():
            target = dest / m.name
            if m.isdir(): target.mkdir(parents=True, exist_ok=True); continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with t.extractfile(m) as src, open(target, "wb") as out: shutil.copyfileobj(src, out)

def package_dir(arg, work):
    p = Path(arg)
    if p.is_file() and p.suffix.lower() == ".vpk":
        dest = work / "package"; dest.mkdir(parents=True)
        with zipfile.ZipFile(p) as z:
            for info in z.infolist():
                n = info.filename
                if not n.startswith("shadercache/") or n.endswith("/"): continue
                if ".." in Path(n).parts or n.startswith("/"): raise SystemExit(f"refill: refusing VPK member {n!r}")
                target = dest / n; target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(target, "wb") as out: shutil.copyfileobj(src, out)
        return dest / "shadercache"
    if (p / "shadercache").is_dir(): return p / "shadercache"
    if p.is_dir(): return p
    raise SystemExit(f"refill: no package shadercache at {arg}")

def pulled_dir(arg, work, i):
    """the folder that plays ux0:data/smg/shadercache (holding <key>/v3), or a <key> folder, or a v3 folder"""
    p = Path(arg)
    if p.is_file():
        dest = work / f"pulled{i}"; dest.mkdir(parents=True); safe_extract_tgz(p, dest); p = dest
    if not p.is_dir(): raise SystemExit(f"refill: {arg} is neither a folder nor an archive")
    return p

def find_v3(root, key):
    """(v3 folder for this key or None, folder that holds the lists or None)"""
    root = Path(root)
    cands = [root / key / "v3", root / "v3"] + sorted(root.glob(f"*/{key}/v3")) + sorted(root.glob(f"*/*/{key}/v3"))
    if root.name == "v3": cands.insert(0, root)
    for c in cands:
        if c.is_dir():
            lists = c.parent.parent if c.parent.name == key else None
            return c, lists
    lists = root if (root / "vgx_shaderlist.bin").exists() or (root / "vgx_stagewarm.bin").exists() else None
    for c in sorted(root.glob("*/vgx_stagewarm.bin")): lists = lists or c.parent
    return None, lists

def stage_image(b, kind):
    """ShaderPack::StageImage (platform/vgx/vita/vgx_shader_pack.h): vitaGL's cache record = u32 n + n u32 matrix indices + a GXP of
    the right stage, size at +8 (end padding 0..3), parameter table inside it. A file cut short by a copy fails this."""
    u32 = lambda o: struct.unpack_from("<I", b, o)[0]
    if len(b) < 4: return False
    matrices = u32(0); prefix = 4 + matrices * 4
    if prefix > len(b) or len(b) - prefix < 156: return False
    g = prefix; size = u32(g + 8)
    if b[g:g + 4] != b"GXP\0" or size < 156 or ((size + 3) & ~3) != len(b) - prefix or (u32(g + 20) & 1) != (0 if kind == "v" else 1): return False
    count, base = u32(g + 36), 40 + u32(g + 40)
    if count and ((base & 3) or base < 156 or base > size or count * 16 > size - base): return False
    return all(u32(4 + i * 4) < count for i in range(matrices))

def inventory(v3, label, bad):
    files = {}
    if v3 is None: return files
    for path in sorted(Path(v3).rglob("*")):
        if path.is_dir(): continue
        rel = path.relative_to(v3).as_posix()
        if path.is_symlink(): bad.append(f"{label}\t{rel}\tsymlink"); continue
        if not canonical(rel): bad.append(f"{label}\t{rel}\tnot a canonical cache name (ignored)"); continue
        data = path.read_bytes()
        if len(data) < 8 or len(data) > MAX_RECORD: bad.append(f"{label}\t{rel}\tsize {len(data)} (ignored)"); continue
        if not stage_image(data, rel[0]): bad.append(f"{label}\t{rel}\tnot a whole {rel[0]} cache record (cut short?) (ignored)"); continue
        files[rel] = (path, len(data), hashlib.sha256(data).hexdigest())
    return files

# ---- the identity ------------------------------------------------------------------------------------------------------------------
def identity_from_log(path):
    ident = None
    for line in Path(path).read_text(errors="replace").splitlines():
        m = re.search(r"compiled-shader cache: (\S+) \(key of: (.*)\)\s*$", line)
        if m: ident = (m.group(1), m.group(2))
    if not ident: raise SystemExit(f"refill: no 'compiled-shader cache: ... (key of: ...)' line in {path}")
    return ident[1], ident[0]

def identity_from_source(tree):
    host = (tree / "platform/vgx/vita/vgx_host_galaxy_vita.cpp").read_text()
    abi = re.search(r'kHostShaderAbi = "([^"]*)"', host).group(1)
    gen = re.search(r"kShaderGenVersion = (\d+)", (tree / "platform/vgx/src/vgx_shadergen.h").read_text()).group(1)
    vgl = None
    for nin in ("/build/vita-vgx-runtime/build.ninja", "/build/vita-vgx-game/build.ninja"):
        if os.path.exists(nin):
            m = re.search(r'-DGXV_VGL_ID="([^"]*)"', open(nin, errors="replace").read())
            if m: vgl = m.group(1); break
    if not vgl: vgl = re.search(r'#define GXV_VGL_ID "([^"]*)"', host).group(1)
    return f"{vgl}|{abi}|gen {gen}"

# ---- the game's environment defaults (for the names the generator makes) -------------------------------------------------------------
def entry_defaults(tree):
    """setenv("X", "v", 0) and for (const char* name : {...}) setenv(name, "v", 0) of vita/vgx-game/entry.cpp, first one wins"""
    text = (tree / "vita/vgx-game/entry.cpp").read_text()
    text = re.sub(r"//[^\n]*", "", text)
    found, conflicts = [], []
    for m in re.finditer(r'setenv\(\s*"([A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*0\s*\)', text): found.append((m.start(), m.group(1), m.group(2)))
    for m in re.finditer(r'for\s*\(\s*const\s+char\s*\*\s*name\s*:\s*\{([^}]*)\}\s*\)\s*\{?\s*setenv\(\s*name\s*,\s*"([^"]*)"\s*,\s*0\s*\)', text, re.S):
        for n in re.findall(r'"([A-Z0-9_]+)"', m.group(1)): found.append((m.start(), n, m.group(2)))
    env = {}
    for _, k, v in sorted(found):
        if k in env and env[k] != v: conflicts.append(f"{k}: {env[k]} (kept) / {v}")
        env.setdefault(k, v)
    return env, conflicts

def build_tool(cmd, out, what):
    if out.exists(): return out
    out.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(cmd + ["-o", str(out)], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f"refill: building {what} failed:\n{r.stderr[-3000:]}")
    return out

# ---- lists -------------------------------------------------------------------------------------------------------------------------
def read_shaderlist(path):
    d = Path(path).read_bytes()
    if len(d) < 24 or d[:8] != b"VGXSHL01": return None
    vs, fs, gen, _ = struct.unpack_from("<IIII", d, 8)
    o, keys, n = 24, {1: [], 2: []}, {1: 0, 2: 0, 3: 0, 4: 0}
    progs, variants = set(), set()
    while o + 4 <= len(d):
        t = struct.unpack_from("<I", d, o)[0]; o += 4
        size = {1: vs, 2: fs, 3: 8, 4: 16}.get(t)
        if size is None or o + size > len(d): return {"header": (vs, fs, gen), "damaged": True, "n": n}
        if t in (1, 2): keys[t].append(d[o:o + size])
        elif t == 3:
            a, b = struct.unpack_from("<II", d, o)
            if a < len(keys[1]) and b < len(keys[2]): progs.add((keys[1][a], keys[2][b]))
        else:
            a, b, c, e = struct.unpack_from("<IIII", d, o)
            if a < len(keys[1]) and b < len(keys[2]): variants.add((keys[1][a], keys[2][b], c, e))
        n[t] += 1; o += size
    return {"header": (vs, fs, gen), "damaged": False, "n": n, "vs": set(keys[1]), "fs": set(keys[2]), "progs": progs, "variants": variants,
            "bytes": len(d)}

def read_stagewarm(path):
    d = Path(path).read_bytes()
    if len(d) < 8 or d[:8] != b"VGXSTW01": return None
    n = (len(d) - 8) // 24
    return [struct.unpack_from("<IIQQ", d, 8 + 24 * i) for i in range(n)]   # (stage, lb, vs, fs)

def stage_name(k):
    return "MENU" if k == MENU_KEY else "PLAYER" if k == PLAYER_KEY else "%08x" % k

def stage_key_of(name):
    """perf.cpp StageKeyOf: FNV-1a of the stage name (what the log prints as 'stage 099e9a42')"""
    return int(fnv1a(name), 16)

def read_ply(path, magic=b"VGXPLY01", max_recs=PLAYER_MAX):
    """the player list file (GXV_PLAYERFILE): 'VGXPLY01', u32 count, u32 FNV sum over the records, then 24-byte records; None if not valid.
    The stage journal (seedrun31 A3, <stage list>.jnl) has the same layout with magic 'VGXSTJ01' and at most 8,192 records (vgx_backend_gl.cpp PlyRead)."""
    d = Path(path).read_bytes()
    if len(d) < 16 or d[:8] != magic: return None
    count, csum = struct.unpack_from("<II", d, 8)
    if count > max_recs or len(d) != 16 + 24 * count: return None
    h = 2166136261
    for b in d[16:]: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    if h != csum: return None
    return [struct.unpack_from("<IIQQ", d, 16 + 24 * i) for i in range(count)]

def read_journal(path):
    return read_ply(path, b"VGXSTJ01", JOURNAL_MAX)

def read_drawn(path):
    """(harvprep31) a harvest leg's draw evidence (<stage list>.drawn, GXV_SEEDWALK=1): the player file's layout with magic 'VGXSTD01'; lb holds the draw bits only"""
    return read_ply(path, b"VGXSTD01", DRAWN_MAX)

def draw_key(st, lb, vs, fs):
    return (st, vs, fs, lb & stagetags.DRAW)

def prune_by_evidence(recs, drawn_by, have_evidence, cap=STAGE_MAX_RECS, mode="fill"):
    """(harvprep31) a stage over `cap` own records is pruned back by the harvest's draw evidence. drawn_by {(stage, vs, fs, draw): legs that drew it}.
    Kept: every record a leg drew (all of them: a drawn record is never dropped), then the records no leg drew in list order while there is room (fill) or none (all).
    -> (records in their list order, dropped [(rec, why)], per-stage report lines, failures, rank rows [(stage, legs, rec)])"""
    per = {}
    for i, (st, lb, vs, fs) in enumerate(recs):
        if st == stagetags.STAMP_KEY or lb & stagetags.CANDRAW: continue
        per.setdefault(st, []).append(i)
    drop, lines, fails, rank = set(), [], [], []
    for st, idx in sorted(per.items()):
        if len(idx) <= cap: continue
        if mode == "off" or not have_evidence:
            fails.append(f"CAP: stage {stage_name(st)}: {len(idx)} own records, over the release's {cap}, and "
                         + ("--prune off" if mode == "off" else "no harvest folder holds draw evidence (vgx_stagewarm.bin.drawn: legs of a GXV_SEEDWALK=1 build of harvprep31 or later)"))
            continue
        legs = {i: len(drawn_by.get(draw_key(*recs[i]), ())) for i in idx}
        drawn = [i for i in idx if legs[i]]
        undrawn = [i for i in idx if not legs[i]]
        for i in idx: rank.append((st, legs[i], recs[i]))
        if len(drawn) > cap:
            fails.append(f"CAP: stage {stage_name(st)}: {len(drawn)} records drawn by the harvest, more than the release's {cap} a stage: nothing drawn is dropped "
                         f"(PRUNE-RANK.tsv ranks them by the legs that drew them; a decision is needed)")
            continue
        room = cap - len(drawn) if mode == "fill" else 0
        keep_undrawn = set(undrawn[:room])
        gone = [i for i in undrawn if i not in keep_undrawn]
        drop.update(gone)
        hist = {}
        for i in drawn:
            b = "1" if legs[i] == 1 else "2-3" if legs[i] <= 3 else "4-9" if legs[i] <= 9 else "10+"
            hist[b] = hist.get(b, 0) + 1
        lines.append(f"prune: stage {stage_name(st)}: {len(idx)} own records, over the release's {cap}: {len(drawn)} drawn by the harvest (kept; by legs that drew them: "
                     + ", ".join(f"{k} leg(s) {hist[k]}" for k in ("1", "2-3", "4-9", "10+") if k in hist)
                     + f"), {len(undrawn)} drawn by no leg: {len(keep_undrawn)} kept, {len(gone)} DROPPED (--prune {mode}) -> {len(idx) - len(gone)} records")
    kept = [r for i, r in enumerate(recs) if i not in drop]
    return kept, [(recs[i], "drawn by no harvest leg") for i in sorted(drop)], lines, fails, rank

def side_file(path, reader):
    """the game's rule for the player file and the journal (PlayerFileLoad, StageJournalLoad): a complete <path>.tmp first (a rename that did not happen), else <path>.
    -> (records or None, the path read or None when neither exists)"""
    for p in (Path(str(path) + ".tmp"), Path(path)):
        if p.exists():
            r = reader(p)
            if r is not None: return r, p
    return None, (Path(path) if Path(path).exists() else None)

def merge_stage_lists(lists):
    """(fix31) the union of several stage lists [(label, [(stage, lb, vs, fs)])], merged on the draw (stagetags.normalize: stage, key hashes, draw bits, menu class;
    hints that disagree become unhinted, slow if any copy was, can-draw copies of an own draw dropped), stamp records left out, first place kept.
    -> (records, [(label, records read, draws new to the union)])"""
    union, srcs, seen = [], [], set()
    for label, r in lists:
        new = 0
        for st, lb, vs, fs in r:
            if st == stagetags.STAMP_KEY: continue   # (seedrun31b: an input's stamp; the list written gets its own)
            union.append((st, lb, vs, fs))
            k = (st, vs, fs, lb & (stagetags.DRAW | stagetags.MENUCLS), bool(lb & stagetags.CANDRAW))
            if k not in seen: seen.add(k); new += 1
        srcs.append((label, len(r), new))
    return stagetags.normalize(union), srcs

def stage_caps(recs):
    """(fix31) the game's caps on a stage list, own and can-draw records counted apart as the game counts them (StageAdd / CanDrawAdd).
    -> (records kept: the first ones in list order, drops {kind: {stage: n}}, counts). Kinds: 'own' (2,048 a stage), 'own-mem' (32,768 in all), 'cd' (1,024 a
    stage), 'cd-mem' (16,384 in all), 'file' (65,536 records)."""
    kept, drops = [], {"own": {}, "own-mem": {}, "cd": {}, "cd-mem": {}, "file": {}}
    per_own, per_cd, n_own, n_cd = {}, {}, 0, 0
    for rec in recs:
        st, lb = rec[0], rec[1]
        if st == stagetags.STAMP_KEY: kept.append(rec); continue
        why = None
        if len(kept) >= STAGE_FILE_MAX: why = "file"
        elif lb & stagetags.CANDRAW:
            if n_cd >= CANDRAW_MEM_MAX: why = "cd-mem"
            elif per_cd.get(st, 0) >= CANDRAW_STAGE_MAX: why = "cd"
        elif n_own >= STAGE_MEM_MAX: why = "own-mem"
        elif per_own.get(st, 0) >= STAGE_MAX_RECS: why = "own"
        if why: drops[why][st] = drops[why].get(st, 0) + 1; continue
        kept.append(rec)
        if lb & stagetags.CANDRAW: per_cd[st] = per_cd.get(st, 0) + 1; n_cd += 1
        else: per_own[st] = per_own.get(st, 0) + 1; n_own += 1
    return kept, drops, {"own": n_own, "cd": n_cd, "own_stage_max": max(per_own.values(), default=0), "cd_stage_max": max(per_cd.values(), default=0)}

def cap_failures(drops):
    """the run's failure texts for the drops of stage_caps, one a kind (and stage)"""
    out = []
    for st, n in sorted(drops["own"].items()):
        out.append(f"CAP: stage {stage_name(st)}: {n} record(s) over the game's {STAGE_MAX_RECS} a stage dropped (the game's StageAdd drops them too and logs 'cap-full:')")
    if drops["own-mem"]:
        out.append(f"CAP: {sum(drops['own-mem'].values())} own record(s) dropped: the stage records are more than the game keeps in memory ({STAGE_MEM_MAX}): "
                   f"it would drop the rest of every load ('cap-full:' in its log)")
    for st, n in sorted(drops["cd"].items()):
        out.append(f"CAP: stage {stage_name(st)}: {n} can-draw record(s) over the game's {CANDRAW_STAGE_MAX} a stage dropped (CanDrawAdd drops them too and logs 'cap-full:')")
    if drops["cd-mem"]:
        out.append(f"CAP: {sum(drops['cd-mem'].values())} can-draw record(s) over the {CANDRAW_MEM_MAX} the game keeps in memory dropped (CanDrawAdd logs 'cap-full:')")
    if drops["file"]:
        out.append(f"CAP: {sum(drops['file'].values())} record(s) over the stage file's {STAGE_FILE_MAX} dropped")
    return out

def ply_count(path):
    d = Path(path).read_bytes()
    return struct.unpack_from("<I", d, 8)[0] if len(d) >= 16 and d[:8] == b"VGXPLY01" else None

def bytes_equal(a, b):
    """the byte-compare path of compiled files: the very bytes, read from both files (not a hash)"""
    return Path(a).read_bytes() == Path(b).read_bytes()

def closure_check(recs, kmap, have):
    """seedgate31 closure: for every stage record (stage, lb, vs hash, fs hash) both key recipes in the key list (kmap {'v'|'f': {hash: file name}}) and both compiled
    files in `have` (names). -> (open records [(rec, [reasons])], names missing a file, hashes without a recipe)"""
    open_recs, nofile, norecipe = [], set(), set()
    for rec in recs:
        if rec[0] == stagetags.STAMP_KEY: continue   # (seedrun31b: the tags' stamp record, not a draw)
        why = []
        for kind, h in (("v", rec[2]), ("f", rec[3])):
            name = kmap[kind].get(h)
            if name is None: why.append(f"{kind} {h:016x}: no key recipe in the key list"); norecipe.add((kind, h))
            elif name not in have: why.append(f"{kind} {h:016x}: no compiled file {name}"); nofile.add(name)
        if why: open_recs.append((rec, why))
    return open_recs, nofile, norecipe

def tour_stage_names(path):
    names = []
    for line in Path(path).read_text().split():
        for item in line.split(","):
            n = item.strip().split(":")[0]
            if n and not n.startswith("#") and n not in names: names.append(n)
    return names

# ---- main --------------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True); ap.add_argument("--pulled", action="append", default=[])
    ap.add_argument("--out", required=True); ap.add_argument("--identity"); ap.add_argument("--log")
    ap.add_argument("--expect", action="append", default=[]); ap.add_argument("--source")
    ap.add_argument("--tools-src", default=None, help="tree whose tools/ and tests/ build the pack tools (default: --source)")
    ap.add_argument("--list", choices=("package", "card"), default="card"); ap.add_argument("--prefer", choices=("package", "card"), default="package")
    ap.add_argument("--stagewarm", choices=("card", "union", "package"), default="card")
    ap.add_argument("--seed-policy", choices=("needed", "all", "none"), default="needed")
    ap.add_argument("--tour-stages", default=None, help="file: one stage name per line that the tour visited (default: tour_legs.txt next to this script)")
    ap.add_argument("--max-entries", type=int, default=MAX_PACK_ENTRIES); ap.add_argument("--max-bytes", type=int, default=MAX_PACK_BYTES)
    ap.add_argument("--old-pack", choices=("list", "off"), default="list",
                    help="list (default, needs --source): take records the lists need from the package's old stages.pack")
    ap.add_argument("--misslog", action="append", default=[], help="a collect-tour log with 'pack-miss:' lines (GXV_PACK_MISSLOG=1); every key must have a file")
    ap.add_argument("--only-logged", action="store_true", help="with --misslog: the pack holds exactly the logged keys (no seed leftovers, no old-pack records)")
    ap.add_argument("--keep-work", action="store_true")
    ap.add_argument("--harvest", action="append", default=[], help="an emulator harvest folder (card delta + logs): lists and records used, files only when byte-proven")
    ap.add_argument("--profile-log", action="append", default=[], help="a log with the game's 'profile: word=' line (checked against the release tree's word)")
    ap.add_argument("--release-word", default=None, help="the release's profile word (16 hex) instead of the one made from --source")
    ap.add_argument("--require-profile", action="store_true", help="--pulled folders' and --misslog logs must also carry a profile word")
    ap.add_argument("--min-overlap", type=int, default=25, help="texts both the console inputs and a harvest hold that prove the emulator's compiler identical")
    ap.add_argument("--harvest-files", choices=("byte-proven", "lists-only"), default="byte-proven")
    ap.add_argument("--closure", choices=("require", "report"), default="require")
    ap.add_argument("--pack-overflow", choices=("fail", "usage"), default="fail", help="more texts than the RAM pack holds: fail, or the most widely used ones go into it")
    ap.add_argument("--tool-cache", default=None, help="a folder that keeps the built helper tools between runs (their names carry a hash of their sources); default: built in <out>/_work")
    ap.add_argument("--gate-only", action="store_true", help="gate the package's own seed: no pull, no merge")
    ap.add_argument("--prune", choices=("fill", "all", "off"), default="fill",
                    help="(harvprep31) a stage over the release's 2,048 own records: drop records no harvest leg drew until it fits (fill), all of them (all), or fail (off)")
    a = ap.parse_args()
    if a.gate_only:
        if a.pulled or a.harvest: raise SystemExit("refill: --gate-only takes no --pulled / --harvest")
        a.seed_policy, a.list, a.stagewarm, a.old_pack = "all", "package", "package", "off"
    out = Path(a.out)
    if not a.source: raise SystemExit("refill: --source <the tree the eboot was built from> is required (the generator names the texts)")
    if a.tools_src is None: a.tools_src = a.source
    if out.exists(): raise SystemExit(f"refill: {out} exists; give a new folder")
    # the profile gate: BEFORE anything is read into the output. A harvest of another profile is refused whole (exit 4).
    rel_word, rel_values, rel_items, rel_gen = release_profile(Path(a.source))
    if a.release_word:
        if not re.fullmatch(r"[0-9a-fA-F]{16}", a.release_word): raise SystemExit("refill: --release-word must be 16 hex digits")
        rel_word = a.release_word.lower()
    refusals = []
    for h in a.harvest:
        if not Path(h).is_dir(): raise SystemExit(f"refill: --harvest {h} is not a folder")
        refusals += profile_problems(f"harvest {h}", logs_in(h) + [Path(x) for x in a.profile_log], rel_word, rel_values, True)
    for x in a.profile_log:
        if not Path(x).is_file(): raise SystemExit(f"refill: --profile-log {x} is not a file")
    if a.profile_log and not a.harvest: refusals += profile_problems("profile log", [Path(x) for x in a.profile_log], rel_word, rel_values, True)
    for pth in a.pulled:
        if Path(pth).is_dir(): refusals += profile_problems(f"pulled {pth}", logs_in(pth), rel_word, rel_values, a.require_profile)
    if a.misslog: refusals += profile_problems("miss logs", [Path(x) for x in a.misslog], rel_word, rel_values, a.require_profile)
    refusals = sorted(set(refusals))
    if refusals:
        print(f"refill: REFUSED: the input is not of the release's shader profile (word {rel_word}, from {'--release-word' if a.release_word else a.source}). Nothing was written.")
        for r in refusals: print("  " + r)
        return 4
    out.mkdir(parents=True); work = out / "_work"; work.mkdir()
    failed = []

    # identity and key
    if a.identity: identity, src = a.identity, "--identity"
    elif a.log: identity, logged = identity_from_log(a.log); src = f"log {a.log} ({logged})"
    elif a.source: identity, src = identity_from_source(Path(a.source)), f"source {a.source}"
    else: raise SystemExit("refill: give --identity, --log or --source")
    key = fnv1a(identity)
    say(f"identity ({src}): {identity}"); say(f"cache folder key: {key}")
    say(f"profile word of the release tree ({a.source}, generator {rel_gen}, {len(rel_items)} text switches): {rel_word}" + (f"  (--release-word)" if a.release_word else ""))
    say("  checked: " + ("; ".join(x for x in (f"{len(a.harvest)} harvest folder(s)" if a.harvest else "", f"{len(a.profile_log)} profile log(s)" if a.profile_log else "",
                                              f"{len(a.pulled)} pulled" if a.pulled else "", f"{len(a.misslog)} miss log(s)" if a.misslog else "") if x) or "no log given (--gate-only / package alone)"))
    if a.log:
        logged_key = logged.rstrip("/").rsplit("/", 1)[-1]
        if logged_key != key: raise SystemExit(f"refill: the log's folder {logged_key} is not FNV-1a(identity) {key}")

    # inputs
    pkg = package_dir(a.package, work)
    pkg_v3, _ = find_v3(pkg, key)
    bad = []
    pkg_files = inventory(pkg_v3, "package", bad)
    if pkg_v3 is None: say(f"package: NO seed folder {key}/v3 (a new key: the card's files alone make the new seed)")
    say(f"package seed {pkg}: {len(pkg_files)} .gxp ({sum(1 for r in pkg_files if r[0] == 'v')} v, {sum(1 for r in pkg_files if r[0] == 'f')} f)")
    pulls = []
    for i, p in enumerate(a.pulled):
        root = pulled_dir(p, work, i)
        v3, lists = find_v3(root, key)
        files = inventory(v3, f"pulled{i}", bad)
        others = sorted(x.name for x in root.rglob("*") if x.is_dir() and re.fullmatch(r"[0-9a-f]{8}", x.name) and x.name != key)
        say(f"pulled[{i}] {p}: {len(files)} .gxp for {key}" + (f"; other key folders ignored: {', '.join(others)}" if others else "")
            + ("" if v3 else f"  (NO {key}/v3 in it)"))
        pulls.append((p, files, lists))
    emus = []     # (seedgate31) emulator harvests: [(folder, files, lists)]; their files wait for the byte-compare below
    for i, p in enumerate(a.harvest):
        root = pulled_dir(p, work, 100 + i)
        v3h, lists = find_v3(root, key)
        files = inventory(v3h, f"harvest{i}", bad)
        say(f"harvest[{i}] {p}: {len(files)} .gxp for {key}" + ("" if v3h else f"  (NO {key}/v3 in it)") + ("" if lists else "  (no lists)"))
        emus.append((p, files, lists))
    if bad:
        (out / "IGNORED-FILES.txt").write_text("\n".join(bad) + "\n")
        say(f"ignored files (not canonical / size / not a whole GXP record): {len(bad)} -> IGNORED-FILES.txt")

    # merge
    merged, origin, conflicts = {}, {}, []
    for rel, info in pkg_files.items(): merged[rel] = info; origin[rel] = "package"
    for i, (_, files, _) in enumerate(pulls):
        for rel, info in files.items():
            if rel not in merged: merged[rel] = info; origin[rel] = f"pulled{i}"; continue
            if merged[rel][2] != info[2]:
                conflicts.append(f"{rel}\t{origin[rel]} {merged[rel][1]} bytes\tpulled{i} {info[1]} bytes")
                if a.prefer == "card": merged[rel] = info; origin[rel] = f"pulled{i}"
    # (seedgate31) the emulator's files: a text both sides hold must be byte-identical; only then may the harvest's other files ship
    if emus:
        overlap, differ, emu_new, among = 0, [], {}, []
        for i, (hp, hfiles, _) in enumerate(emus):
            for rel, info in sorted(hfiles.items()):
                if rel in merged:
                    overlap += 1
                    if not bytes_equal(merged[rel][0], info[0]): differ.append(f"{rel}\t{origin[rel]} {merged[rel][1]} bytes\tharvest{i} {info[1]} bytes")
                elif rel in emu_new:
                    if not bytes_equal(emu_new[rel][0][0], info[0]): among.append(f"{rel}\t{emu_new[rel][1]}\tharvest{i}")
                else: emu_new[rel] = (info, f"harvest{i}")
        # (strk31) what only the console has: a .gxp is written only by a real compile, so an emulator walk can miss programs the console compiled (Strikers: 21 of them).
        # Console files are the reference: they are always kept; this lists the ones no harvest holds, so the gain of the console pull can be read.
        harvested = set().union(*[set(hf) for _, hf, _ in emus])
        console_only = sorted(r for r in merged if origin[r].startswith("pulled") and r not in harvested)
        (out / "CONSOLE-ONLY.txt").write_text("".join(f"{r}\t{origin[r]}\t{merged[r][1]} bytes\n" for r in console_only))
        say(f"console-only files (compiled on the console, in no emulator harvest): {len(console_only)} of {sum(1 for r in merged if origin[r].startswith('pulled'))} pulled -> CONSOLE-ONLY.txt")
        state = "BROKEN" if differ or among else "UNPROVEN" if overlap < a.min_overlap else "PROVEN"
        (out / "COMPILER-IDENTITY.txt").write_text(f"{state}: {overlap} texts in both the console inputs and the harvest(s) (needed {a.min_overlap}); {len(differ)} differ; "
                                                   f"{len(among)} differ between harvests\n" + "".join(f"DIFF {d}\n" for d in differ) + "".join(f"DIFF-HARVESTS {d}\n" for d in among))
        say(f"compiler identity (byte compare, emulator against console): {state}: {overlap} shared texts, {len(differ)} differ, {len(among)} differ between harvests -> COMPILER-IDENTITY.txt")
        if state == "PROVEN":
            for rel, (info, label) in emu_new.items(): merged[rel] = info; origin[rel] = label
            say(f"  {len(emu_new)} compiled files of the emulator harvest taken (the emulator's compiler gave the console's bytes on every shared text)")
        else:
            say(f"  the emulator's {len(emu_new)} files that the console inputs lack are NOT taken: lists and records only; the files must come from a console compile job (GXV_KEYS27_COMPILELIST)")
            if a.harvest_files != "lists-only":
                failed.append(f"compiler identity {state}: the emulator's compiled files are unproven ({overlap} shared texts, {len(differ)} differ); "
                              "use --harvest-files lists-only to take the lists alone, and compile the files on the console")
    new = [r for r in merged if origin[r] != "package"]
    say(f"merged: {len(merged)} .gxp ({sum(1 for r in merged if r[0] == 'v')} v, {sum(1 for r in merged if r[0] == 'f')} f); "
        f"{len(new)} new from the console ({sum(1 for r in new if r[0] == 'v')} v, {sum(1 for r in new if r[0] == 'f')} f), "
        f"{sum(info[1] for info in merged.values())} bytes")
    if conflicts:
        (out / "CONFLICTS.txt").write_text("\n".join(conflicts) + "\n")
        say(f"same name, different bytes: {len(conflicts)} (kept: {a.prefer}'s) -> CONFLICTS.txt")
    (out / "NEW-FROM-CONSOLE.txt").write_text("".join(f"{r}\n" for r in sorted(new)))

    # (packrefill25) the collect tour's miss log: the keys the final-switch run asked for
    logged = None
    if a.only_logged and not a.misslog: raise SystemExit("refill: --only-logged needs --misslog")
    if a.misslog:
        sys.path[:0] = [str(HERE), str(HERE.parent / "pack25")]
        import misskeys
        mk, infos = misskeys.read_logs(a.misslog)
        logged = set(mk)
        nobanner = [i.path for i in infos if not i.banner]
        say(f"miss log: {len(logged)} distinct keys from {len(infos)} log(s) ({sum(1 for k in logged if k[0] == 'v')} vertex, {sum(1 for k in logged if k[0] == 'f')} fragment)"
            + (f"; NO GXV_PACK_MISSLOG banner in: {', '.join(nobanner)}" if nobanner else ""))
        if nobanner: failed.append("a miss log without the GXV_PACK_MISSLOG banner (the run did not log)")
        if not logged: failed.append("the miss logs hold no 'pack-miss:' line")
        if a.only_logged:
            unasked = sorted(r for r in merged if r not in logged)
            for r in unasked: del merged[r]; del origin[r]
            (out / "UNASKED-FILES.txt").write_text("".join(f"{r}\n" for r in unasked))
            a.old_pack = "off"; a.seed_policy = "all"   # (the filter above is the policy)
            say(f"--only-logged: {len(unasked)} files nobody asked for dropped (UNASKED-FILES.txt), {len(merged)} remain; the old package pack is not consulted")

    sc = out / "shadercache"; v3 = sc / key / "v3"
    for kind in "vf": (v3 / kind).mkdir(parents=True, exist_ok=True)

    # lists
    pkg_list = pkg / "vgx_shaderlist.bin"; pkg_stw = pkg / "vgx_stagewarm.bin"
    card_lists = [(p, l) for p, _, l in pulls + emus if l is not None]
    pl = read_shaderlist(pkg_list) if pkg_list.exists() else None
    cl = None
    for p, l in card_lists:
        if (l / "vgx_shaderlist.bin").exists():
            cand = (l / "vgx_shaderlist.bin", read_shaderlist(l / "vgx_shaderlist.bin"))
            if cl is None or (cand[1] and not cand[1].get("damaged") and (not cl[1] or cl[1].get("damaged") or len(cand[1]["progs"]) > len(cl[1]["progs"]))): cl = cand
    def lsum(name, x):
        if not x: return f"{name}: none"
        if x.get("damaged"): return f"{name}: header {x['header']} DAMAGED after {x['n']}"
        return (f"{name}: header (vs {x['header'][0]}, fs {x['header'][1]}, gen {x['header'][2]}), {len(x['vs'])} vertex keys, "
                f"{len(x['fs'])} fragment keys, {len(x['progs'])} programs, {len(x['variants'])} variants, {x['bytes']} bytes")
    say(lsum("package warm-up list", pl)); say(lsum("card warm-up list", cl[1] if cl else None))
    if pl and cl and cl[1] and not cl[1].get("damaged") and not pl.get("damaged"):
        if pl["header"] != cl[1]["header"]: say("  the card's list is of ANOTHER generator/key size (gen bump?): only the card's list fits the new seed")
        else:
            say(f"  card list beyond the package's: +{len(cl[1]['vs'] - pl['vs'])} vertex keys, +{len(cl[1]['fs'] - pl['fs'])} fragment keys, "
                f"+{len(cl[1]['progs'] - pl['progs'])} programs, +{len(cl[1]['variants'] - pl['variants'])} variants; "
                f"package-only programs: {len(pl['progs'] - cl[1]['progs'])}")
    chosen = None
    if a.list == "card":
        if not cl: failed.append("--list card, but no card list was pulled"); chosen = pkg_list if pkg_list.exists() else None
        else: chosen = cl[0]
    else:
        chosen = pkg_list if pkg_list.exists() else (cl[0] if cl else None)
        if pl and cl and cl[1] and pl["header"] != cl[1]["header"]:
            chosen = cl[0]; say("  -> taking the card's list (the package's is another generator's)")
    if chosen: shutil.copyfile(chosen, sc / "vgx_shaderlist.bin"); say(f"vgx_shaderlist.bin <- {chosen}")
    else: say("vgx_shaderlist.bin: none (no list in the inputs)")

    # the stage list: the card's own (default: the clean refill's: only records of this build's key hashes), or the union with the package's
    # (package records first), plus the player list's file (vgx_stagewarm.bin.ply, GXV_PLAYERFILE) as PLYR records
    ply_over = []
    inputs = []
    if a.stagewarm in ("union", "package"): inputs.append(("package", pkg_stw, "list"))
    if a.stagewarm in ("union", "card"):
        for p, l in card_lists:
            inputs.append((f"card {p}", l / "vgx_stagewarm.bin", "list"))
            inputs.append((f"card {p} (.jnl)", l / "vgx_stagewarm.bin.jnl", "jnl"))   # (fix31: the journal, read as the game reads it)
            inputs.append((f"card {p} (.ply)", l / "vgx_stagewarm.bin.ply", "ply"))
    lists = []
    for label, path, kind in inputs:
        if kind == "list":
            if not path.exists(): continue
            r = read_stagewarm(path)
        else:
            r, used = side_file(path, read_journal if kind == "jnl" else read_ply)
            if used is None: continue
            if r is None and kind == "ply" and (ply_count(used) or 0) > PLAYER_MAX: ply_over.append(f"{label}: {ply_count(used)} records")
            if r is not None and used.name.endswith(".tmp"): label += " (its complete .tmp)"
            path = used
        if r is None: say(f"{label} stage list {path}: not a valid {dict(list='VGXSTW01', jnl='VGXSTJ01', ply='VGXPLY01')[kind]} file, ignored"); continue
        if kind == "ply": r = [(PLAYER_KEY, lb, vs, fs) for _, lb, vs, fs in r]
        lists.append((label, r))
    recs, srcs = merge_stage_lists(lists)
    sources = [f"{label}: {n} records, {new} new draws" for label, n, new in srcs]
    # (harvprep31) the harvest legs' draw evidence, and the prune of a stage over the release's cap
    drawn_by, ev_legs = {}, 0
    for p, _, l in emus:
        if l is None: continue
        ev, used = side_file(l / "vgx_stagewarm.bin.drawn", read_drawn)
        if used is None: continue
        if ev is None: say(f"harvest {p}: draw evidence {used}: not a valid VGXSTD01 file, ignored"); continue
        ev_legs += 1
        for st, lb, vs, fs in ev: drawn_by.setdefault(draw_key(st, lb, vs, fs), set()).add(str(p))
    if a.harvest: say(f"draw evidence (harvprep31): {ev_legs} of {len(a.harvest)} harvest folder(s) hold vgx_stagewarm.bin.drawn, {len(drawn_by)} distinct draws")
    if recs:
        recs, pdropped, plines, pfails, prank = prune_by_evidence(recs, drawn_by, ev_legs > 0, STAGE_MAX_RECS, a.prune)
        for ln in plines: say(ln)
        failed.extend(pfails)
        if prank:
            (out / "PRUNE-RANK.tsv").write_text("stage\tlegs that drew it\tlb\tvs\tfs\n" + "".join(f"{stage_name(st)}\t{n}\t{lb:08x}\t{vs:016x}\t{fs:016x}\n"
                                                for st, n, (_, lb, vs, fs) in sorted(prank, key=lambda x: (x[0], -x[1]))))
        if pdropped:
            (out / "PRUNED-records.txt").write_text("".join(f"{stage_name(r[0])}\t{r[1]:08x}\t{r[2]:016x}\t{r[3]:016x}\t{why}\n" for r, why in pdropped))
            say(f"prune: {len(pdropped)} record(s) dropped in all -> PRUNED-records.txt")
    if recs:
        recs, cap_drops, cnt = stage_caps(recs)
        say(f"stage-list merge (on the draw: stagetags normalize): {sum(n for _, n, _ in srcs)} records read from {len(srcs)} list(s) -> {len(recs)} records; "
            f"own {cnt['own']} of {STAGE_MEM_MAX} (largest stage {cnt['own_stage_max']} of {STAGE_MAX_RECS}), can-draw {cnt['cd']} of {CANDRAW_MEM_MAX} "
            f"(largest stage {cnt['cd_stage_max']} of {CANDRAW_STAGE_MAX})")
        # seedrun31b: the renderer has no built-in priority hashes or player seed since seedrun31; they are data, put in here, automatically, on every list
        # (tools/seedrun/stagetags.py legacy: the nine C182/C183 records tagged under their own stage, the old seed as PLYR records at the end, a stamp record)
        recs, tag_rep = stagetags.legacy(recs)
        per_stage = {}
        for st, lb, _, _ in recs:
            if st != stagetags.STAMP_KEY and not lb & stagetags.CANDRAW: per_stage[st] = per_stage.get(st, 0) + 1
        _, after, _ = stage_caps(recs)   # (legacy appends the old seed's PLYR records: the list written must still be under every cap)
        for k, v in after.items():
            for st, n in v.items(): cap_drops[k][st] = cap_drops[k].get(st, 0) + n
        say(f"vgx_stagewarm.bin tags (stagetags.py legacy, automatic): {tag_rep}")
        for p in stagetags.check(recs): failed.append(f"stage list tags: {p}")
        with open(sc / "vgx_stagewarm.bin", "wb") as w:
            w.write(b"VGXSTW01")
            for st, lb, vs, fs in recs: w.write(struct.pack("<IIQQ", st, lb, vs, fs))
        pkg_stages = {s for s, _, _, _ in (read_stagewarm(pkg_stw) or [])} if pkg_stw.exists() else set()
        say(f"vgx_stagewarm.bin (--stagewarm {a.stagewarm}): {len(recs)} records, {len(per_stage)} stage keys ({len(set(per_stage) - pkg_stages)} not in the package's), "
            f"menu records {per_stage.get(MENU_KEY, 0)}, player records {per_stage.get(PLAYER_KEY, 0)}, can-draw records {sum(1 for r in recs if r[1] & stagetags.CANDRAW)}; "
            + "; ".join(sources))
        (out / "STAGEWARM-STAGES.txt").write_text("".join(f"{stage_name(k)}\t{v}\n" for k, v in sorted(per_stage.items())))
        if not per_stage.get(MENU_KEY): failed.append("stage list has no menu records")
        failed.extend(cap_failures(cap_drops))
        for o in ply_over: failed.append(f"CAP: player list over {PLAYER_MAX} records ({o}): the game reads none of it")
        if not per_stage.get(PLAYER_KEY): say("  NOTE: no player (PLYR) records: the tour's pad route never made a draw link a program within 6 s of an input "
                                              "(normal when the warm-up had linked everything before the route ran; the shipped list still warms those programs)")
        tour = tour_stage_names(a.tour_stages or (HERE / "tour_legs.txt"))
        noreg = [n for n in tour if not per_stage.get(stage_key_of(n))]
        say(f"  tour coverage: {len(tour) - len(noreg)} of the {len(tour)} stage names have records" + (f"; NONE for: {', '.join(noreg)}" if noreg else ""))
        if noreg:
            (out / "MISSING-stages.txt").write_text("".join(f"{n}\t{stage_key_of(n):08x}\n" for n in noreg))
            failed.append(f"{len(noreg)} tour stage(s) have no stage-list records (not visited, or the leg crashed): MISSING-stages.txt")
    else:
        failed.append("no stage records at all")

    # the names the generator makes for the lists (--source)
    list_names, genv = {}, {}
    kmap = {"v": {}, "f": {}}     # (seedgate31) the final key list: key hash -> compiled file name, for the closure gate
    if a.source:
        tree = Path(a.source)
        env, envc = entry_defaults(tree)
        srcs = [HERE / "listnames.cpp", tree / "platform/vgx/src/vgx_shadergen.cpp", tree / "platform/vgx/src/vgx_shadergen.h"]
        tag = hashlib.sha256(b"".join(x.read_bytes() for x in srcs)).hexdigest()[:10]   # (the binary's name carries the hash of what it was built from)
        lister = build_tool(["g++", "-std=c++20", "-O1", f"-I{tree}/platform/vgx/src", f"-I{tree}/platform/vgx/third_party",
                             str(srcs[0]), str(srcs[1])], (Path(a.tool_cache) if a.tool_cache else work) / f"listnames-{tag}", "listnames.cpp")
        genv = {k: v for k, v in env.items() if k.startswith("GXV_")}
        say(f"generator env from {tree}/vita/vgx-game/entry.cpp: {len(genv)} GXV_ defaults"
            + (f"; conditional/duplicate ones (first kept): {'; '.join(envc)}" if envc else ""))
        rel_opt = load_opt_switches(tree)
        opt_on = [f"{k}={rel_values.get(k)}" for k, d in rel_opt if rel_values.get(k) and rel_values.get(k) != d]
        say("  text switches (VGX_TEXT_SWITCHES of the tree's vgx_shadergen.h): " + " ".join(f"{k}={rel_values.get(k)}" for k, _ in rel_items)
            + (("  opt: " + " ".join(opt_on)) if opt_on else ""))
        if genv.get("GXV_TEVIN_CANON") != "2": say("  NOTE: this tree's GXV_TEVIN_CANON is not 2: the names are those of the old (mode 0/1) texts")
        runenv = dict(genv, PATH=os.environ.get("PATH", ""))
        todo = [("final warm-up list", sc / "vgx_shaderlist.bin")]
        if cl and not ((sc / "vgx_shaderlist.bin").exists() and (sc / "vgx_shaderlist.bin").read_bytes() == cl[0].read_bytes()): todo.append(("card's warm-up list", cl[0]))
        for label, lst in todo:
            if not lst.exists(): continue
            r = subprocess.run([str(lister), str(lst)], capture_output=True, text=True, env=runenv)
            if r.returncode == 2: say(f"{label}: not this generator's list ({r.stderr.strip()})"); failed.append(f"{label} is another generator's"); continue
            if r.returncode: failed.append(f"listnames failed on {label}"); say(r.stderr.strip()); continue
            fog = subprocess.run([str(lister), str(lst), "--fog"], capture_output=True, text=True, env=runenv)
            used = subprocess.run([str(lister), str(lst), "--used"], capture_output=True, text=True, env=runenv)
            list_names[label] = (r.stdout.split(), fog.stdout.split(), r.stderr.strip(), used.stdout.split())
            if label.startswith("final"):
                clo = subprocess.run([str(lister), str(lst), "--closure"], capture_output=True, text=True, env=runenv)
                if clo.returncode: failed.append("listnames --closure failed on the final key list"); say(clo.stderr.strip())
                for ln in clo.stdout.splitlines():
                    kind, hx, nm = ln.split()
                    kmap[kind][int(hx, 16)] = nm

    # (--old-pack list) files the lists need that neither the loose seed nor the card has, taken from the package's old stages.pack
    # (2.2's pack was built from an older seed: 1,733 of its 2,158 records are not loose files, 223 of them are 2.3 list texts; while the
    # game reads that pack, GXV_SHADERPACK_SEED=1, those are never compiled onto the card)
    needed_all = set().union(*[set(v[0]) for v in list_names.values()]) if list_names else set()
    if a.seed_policy != "all" and list_names:
        drop = [r for r in merged if origin[r] == "package" and (a.seed_policy == "none" or r not in needed_all)]
        for r in drop: del merged[r]; del origin[r]
        kept = sum(1 for r in merged if origin[r] == "package")
        say(f"--seed-policy {a.seed_policy}: {len(drop)} package seed files dropped (the current generator and lists do not name them), {kept} kept; "
            f"{len(merged)} files remain" + ("" if len(drop) == 0 else "  (the dropped ones are texts of the 2.2 defaults or of keys no list holds: STALE-SEED-FILES.txt)"))
        (out / "STALE-SEED-FILES.txt").write_text("".join(f"{r}\n" for r in sorted(drop)))
    old_pack = pkg / key / "stages.pack"
    if a.old_pack == "list" and list_names and old_pack.exists():
        need = needed_all - set(merged)
        d = old_pack.read_bytes(); took = 0
        if d[:8] == b"VGXRAM01":
            n = struct.unpack_from("<I", d, 12)[0]
            for i in range(n):
                st, ln, h, off = struct.unpack_from("<IIQQ", d, PACK_HEADER + PACK_ENTRY * i)
                rel = f"{'v' if st == 1 else 'f'}/{h >> 56:02X}/{h:X}.gxp"
                if rel not in need: continue
                data = d[off:off + ln]
                if len(data) != ln or not stage_image(data, rel[0]): bad.append(f"oldpack\t{rel}\tnot a whole record (ignored)"); continue
                dst = work / "oldpack" / rel; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(data)
                merged[rel] = (dst, ln, hashlib.sha256(data).hexdigest()); origin[rel] = "oldpack"; took += 1
        say(f"old package pack {old_pack}: {took} records the lists need taken (of {len(need)} list names neither seed nor card has)")

    for rel, (path, _, _) in sorted(merged.items()):
        dst = v3 / rel; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(path, dst)

    # the pack
    tools = Path(a.tools_src)
    tdir = Path(a.tool_cache) if a.tool_cache else work
    stag = lambda *fs: hashlib.sha256(b"".join((tools / f).read_bytes() for f in fs)).hexdigest()[:10]
    packer = build_tool(["g++", "-std=c++17", "-O2", f"-I{tools}/platform/vgx/third_party", f"-I{tools}/platform/include", str(tools / "tools/vita_shader_pack.cpp")],
                        tdir / f"vita_shader_pack-{stag('tools/vita_shader_pack.cpp')}", "tools/vita_shader_pack.cpp")
    roundtrip = build_tool(["g++", "-std=c++17", "-O2", f"-I{tools}/platform/vgx/third_party", f"-I{tools}/platform/include", str(tools / "tests/vita_shader_pack_roundtrip.cpp")],
                           tdir / f"vita_shader_pack_roundtrip-{stag('tests/vita_shader_pack_roundtrip.cpp')}", "tests/vita_shader_pack_roundtrip.cpp")
    def pack_cost(names): return PACK_HEADER + sum(PACK_ENTRY + ((merged[n][1] + 3) & ~3) for n in names)
    est = pack_cost(merged)
    say(f"stages.pack: {len(merged)} records, about {est} bytes (limits {a.max_entries} records, {a.max_bytes} bytes; headroom "
        f"{a.max_entries - len(merged)} records, {a.max_bytes - est} bytes); it stays resident in the game's host heap (2.2's full pack: 2,486,484 bytes)")
    if a.max_entries != MAX_PACK_ENTRIES or a.max_bytes != MAX_PACK_BYTES:
        say("  NOTE: custom limits: the game's ShaderPack::kMaxEntries / kMaxBytes (platform/vgx/vita/vgx_shader_pack.h) must have been raised to match, or the game refuses the pack")
    pack_names, pack_v3 = sorted(merged), v3
    if len(merged) > a.max_entries or est > a.max_bytes:
        if a.pack_overflow == "usage" and kmap["v"] and kmap["f"]:
            usage = {}
            for st, lb, vs, fs in recs:
                for kind, h in (("v", vs), ("f", fs)):
                    nm = kmap[kind].get(h)
                    if nm in merged: usage.setdefault(nm, set()).add(st)
            order = sorted(merged, key=lambda n: (-len(usage.get(n, ())), merged[n][1], n))
            pack_names, total = [], PACK_HEADER
            for n in order:
                c = PACK_ENTRY + ((merged[n][1] + 3) & ~3)
                if len(pack_names) < a.max_entries and total + c <= a.max_bytes: pack_names.append(n); total += c
            pack_v3 = work / "packset" / key / "v3"   # (the packer wants the folder to be named <key>/v3)
            for n in pack_names:
                dst = pack_v3 / n; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(merged[n][0], dst)
            say(f"  OVERFLOW (--pack-overflow usage): {len(merged)} texts, the RAM pack takes the {len(pack_names)} used by the most stages ({total} bytes; the least used of them is needed by "
                f"{len(usage.get(pack_names[-1], ()))} stage(s)); the other {len(merged) - len(pack_names)} stay in the indexed seed (loose files)")
        else:
            failed.append("CAP: the merged seed is over the RAM pack's limits (ShaderPack::kMaxEntries / kMaxBytes): no stages.pack written. "
                          "Use --pack-overflow usage (the most widely used texts that fit), or raise the limits in platform/vgx/vita/vgx_shader_pack.h (host heap!)")
            say("  NOT BUILT: over the limits (the loose seed is still complete; without a pack the game reads the files)")
            pack_names = None
    if pack_names is not None:
        r = subprocess.run([str(packer), str(pack_v3), identity, str(sc / key / "stages.pack")], capture_output=True, text=True)
        say("  vita_shader_pack: " + (r.stdout.strip() or r.stderr.strip()))
        if r.returncode: failed.append("vita_shader_pack failed")
        else:
            r = subprocess.run([str(roundtrip), str(pack_v3), identity, str(sc / key / "stages.pack"), str(len(pack_names))], capture_output=True, text=True)
            say("  roundtrip: " + (r.stdout.strip() or f"FAILED (exit {r.returncode})"))
            if r.returncode: failed.append("pack roundtrip failed")
    # the indexed compact seed (tools/c182_compact_seed.py: 8,192 entries, 8 MiB, 4,096 bytes a file): reported, never silently cut
    entries, body, loose, cprobs = c182_compact_seed.cap_report(info[1] for info in merged.values())
    say(f"compact seed (indexed, c182): {entries} entries of {c182_compact_seed.MAX_ENTRIES}, {body} bytes of {c182_compact_seed.MAX_BODY}"
        + (f"; NOTE {loose} file(s) outside 1..{c182_compact_seed.MAX_RECORD} bytes stay loose (not in the indexed seed)" if loose else ""))
    for pr in cprobs: failed.append("CAP: " + pr)

    # expected names
    have = set(merged)
    # (seedgate31) the closure gate: every stage record has both key recipes in the final key list and both compiled files in the merged seed
    if recs and (kmap["v"] or kmap["f"]):
        open_recs, nofile, norecipe = closure_check(recs, kmap, have)
        miss_recipe = sum(1 for _, w in open_recs if any("no key recipe" in x for x in w))
        miss_file = sum(1 for _, w in open_recs if any("no compiled file" in x for x in w))
        say(f"CLOSURE: {len(recs)} stage records ({len(per_stage)} stages); {len(open_recs)} open: {miss_recipe} without a key recipe, {miss_file} without a compiled file "
            f"({len(norecipe)} distinct keys with no recipe, {len(nofile)} distinct files missing)")
        lines = [f"{stage_name(r[0])}\tlb {r[1]:08x}\t" + "; ".join(w) for r, w in open_recs]
        (out / "CLOSURE-REPORT.txt").write_text(f"records {len(recs)}, open {len(open_recs)}, no recipe {miss_recipe}, no file {miss_file}\n" + "".join(l + "\n" for l in lines))
        if nofile: (out / "MISSING-closure.txt").write_text("".join(f"{n}\n" for n in sorted(nofile)))
        if open_recs:
            for l in lines[:6]: say("  open: " + l)
            msg = f"closure: {len(open_recs)} stage record(s) are open (CLOSURE-REPORT.txt)"
            if a.closure == "require": failed.append(msg)
            else: say(f"  NOT GATED (--closure report): {msg}")
    elif recs:
        failed.append("closure: no key list (or none of this generator's) to resolve the stage records' key hashes against")
        say("CLOSURE: cannot be checked: no final key list")
    def coverage(label, names, fname):
        names = sorted(set(names))
        missing = [n for n in names if n not in have]
        frm = {}
        for n in names:
            if n in have: frm[origin[n].rstrip("0123456789")] = frm.get(origin[n].rstrip("0123456789"), 0) + 1
        say(f"{label}: {len(names)} names, {len(names) - len(missing)} present ({', '.join(f'{k} {v}' for k, v in sorted(frm.items()))}), "
            f"{len(missing)} MISSING" + (f" -> {fname}" if missing else ""))
        if missing: (out / fname).write_text("".join(f"{n}\n" for n in missing))
        return missing
    for e in a.expect:
        names = [l.strip() for l in Path(e).read_text().splitlines() if l.strip() and not l.startswith("#")]
        if coverage(f"expected ({e})", names, f"MISSING-{Path(e).stem}.txt"): failed.append(f"names of {e} missing")
    if logged is not None:
        if coverage("expected from the miss log (every key the final-switch tour asked for)", logged, "MISSING-misslog.txt"): failed.append("keys of the miss log have no compiled file (a compile failed, or the card/pull lacks them): MISSING-misslog.txt")
    for label, (names, fognames, note, used) in list_names.items():
        say(f"{label}: {note}")
        slug = "list" if label.startswith("final") else "cardlist"
        coverage(f"expected from the {label} (every key)", names, f"MISSING-{slug}.txt")
        if coverage(f"  of them used by a program or variant (what the warm-up links)", used, f"MISSING-{slug}-used.txt"): failed.append(f"names of the {label} that programs use are missing")
        coverage("  of them fog texts", fognames, f"MISSING-{slug}-fog.txt")

    if bad: (out / "IGNORED-FILES.txt").write_text("\n".join(bad) + "\n")
    # sums and the report
    sums = []
    for p in sorted(sc.rglob("*")):
        if p.is_file() and (p.suffix in (".bin", ".pack")): sums.append(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(out).as_posix()}")
    tree_hash = hashlib.sha256("".join(f"{r}\t{merged[r][2]}\n" for r in sorted(merged)).encode()).hexdigest()
    sums.append(f"{tree_hash}  shadercache/{key}/v3 (sha256 of the sorted 'name<TAB>sha256' lines)")
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    if not a.keep_work: shutil.rmtree(work)
    say(""); say("RESULT: " + ("PASS" if not failed else "CHECKS FAILED: " + "; ".join(failed)))
    say(f"VPK: replace the package's shadercache/ with {sc} (all of it: {key}/v3, {key}/stages.pack, both lists)")
    (out / "REFILL-REPORT.txt").write_text("\n".join(report) + "\n")
    return 0 if not failed else 3

if __name__ == "__main__":
    sys.exit(main())

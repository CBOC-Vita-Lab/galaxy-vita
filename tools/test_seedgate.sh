#!/usr/bin/env bash
# seedgate31 A2: the merge + closure gate (tools/pack24/refill_pack.py, tools/pack25/misskeys.py, tools/c182_compact_seed.py) on simulated pulls (tools/pack24/sim_pull.py:
# fake compiled files with the real names and the real key hashes of a small real key list, tests/seedgate_mklist.cpp). Host only: no docker, no GL, no Vita.
#   1 a closed seed passes (package + console pull + emulator harvest of the release profile)
#   2 a harvest of another profile (GXV_VSOUT=0; and one with no profile line) is REFUSED, exit 4, nothing written
#   3 a record without a key recipe fails closure; a record without a compiled file fails closure
#   4 byte compare: an emulator that compiles other bytes (BROKEN) or too few shared texts (UNPROVEN) -> its files are not taken, the run fails; lists-only passes
#   5 caps: 2,049 records a stage, 33,000 records in all, 4,100 player records, a RAM pack over its limits (and --pack-overflow usage), the compact seed's limits
#     fix31: the merge on the draw (one draw under 7 hints, unhinted and slow in two legs = one record), can-draw records under caps of their own (1,024 a stage,
#     16,384 in all; not counted against the own records'), the leg's journal (.jnl, and a complete .jnl.tmp) read with its stage list
#     harvprep31: a stage over 2,048 pruned by the legs' draw evidence (.drawn): fill / all / off, drawn records over the cap and no evidence fail loud
#   6 --gate-only on the package's own seed; misskeys profile / cover / check
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedgate.XXXXXX"); [[ -n ${KEEP:-} ]] || trap 'rm -rf "$tmp"' EXIT
src="$root/platform/vgx/src"; rp="$root/tools/pack24/refill_pack.py"; sp="$root/tools/pack24/sim_pull.py"; mk="$root/tools/pack25/misskeys.py"
cxx=${CXX:-g++}
fails=0
ok() { echo "ok:   $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
expect_rc() { # name want got
  if [[ $3 == "$2" ]]; then ok "$1 (exit $3)"; else bad "$1: exit $3, wanted $2"; fi
}
expect_in() { # name file text
  if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1: '$3' not in $2"; sed -n '1,400p' "$2" | tail -25; fi
}
"$cxx" -std=c++20 -O1 -I"$src" -I"$root/platform/vgx/third_party" "$root/tools/pack24/listnames.cpp" "$src/vgx_shadergen.cpp" -o "$tmp/listnames" || exit 1
"$cxx" -std=c++20 -O1 -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/seedgate_mklist.cpp" -o "$tmp/mklist" || exit 1
"$cxx" -std=c++20 -O1 -I"$src" "$root/tests/vgx_profile_word.cpp" -o "$tmp/pw" || exit 1
"$tmp/mklist" "$tmp/list.bin" 12 30
# the release's own switch values, as K=V words for the profile tool
relenv=$(python3 -I - "$root" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("rp", sys.argv[1] + "/tools/pack24/refill_pack.py"); rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)
word, values, _, _ = rp.release_profile(sys.argv[1])
print(" ".join(f"--profile-env {k}={v}" for k, v in values.items()))
PY
)
relenv0=${relenv/GXV_VSOUT=1/GXV_VSOUT=0}
[[ $relenv != "$relenv0" ]] || { echo "the release env has no GXV_VSOUT=1"; exit 1; }
sim() { # out [extra args]
  local out=$1; shift
  python3 -I "$sp" --list "$tmp/list.bin" --source "$root" --out "$out" --listnames "$tmp/listnames" "$@" >/dev/null || { bad "sim_pull $out"; }
}
refill() { # name expected-rc out args...
  local name=$1 want=$2 out=$3; shift 3
  python3 -I "$rp" --source "$root" --out "$out" --tool-cache "$tmp/tools" "$@" >"$tmp/$name.txt" 2>&1
  expect_rc "$name" "$want" $?
}
# fixtures: the console's seed (package), the console's pull, the emulator's harvests
sim "$tmp/pkg"
sim "$tmp/pull" --ply 5
# shellcheck disable=SC2086
sim "$tmp/h-ok" --profile-tool "$tmp/pw" $relenv --ply 4
# shellcheck disable=SC2086
sim "$tmp/h-vs0" --profile-tool "$tmp/pw" $relenv0
sim "$tmp/h-noprof"
echo "none of the profile" > "$tmp/h-noprof/run.log"

# 1. closed seed passes
refill pass1 0 "$tmp/o1" --package "$tmp/pkg" --harvest "$tmp/h-ok"
expect_in "closure line" "$tmp/pass1.txt" "CLOSURE:"
expect_in "closure: 0 open" "$tmp/pass1.txt" "0 open: 0 without a key recipe, 0 without a compiled file"
expect_in "compiler identity PROVEN" "$tmp/pass1.txt" "compiler identity (byte compare, emulator against console): PROVEN"
expect_in "release word printed" "$tmp/pass1.txt" "profile word of the release tree"
# seedrun31b: every stage list refill writes carries the first-visit tags (stagetags.py legacy, automatic) and its stamp
expect_in "stage list tagged" "$tmp/pass1.txt" "vgx_stagewarm.bin tags (stagetags.py legacy, automatic)"
if python3 -I "$root/tools/seedrun/stagetags.py" check "$tmp/o1/shadercache/vgx_stagewarm.bin" >/dev/null; then ok "stage list passes stagetags check"; else bad "stage list fails stagetags check"; fi
refill pass1b 0 "$tmp/o1b" --package "$tmp/pkg" --pulled "$tmp/pull" --list card --stagewarm union

# 2. wrong profile word refused, before anything is written
refill vsout0 4 "$tmp/o2" --package "$tmp/pkg" --harvest "$tmp/h-vs0"
expect_in "refusal says the switch" "$tmp/vsout0.txt" "GXV_VSOUT: harvest 0 / release 1"
expect_in "refusal says REFUSED" "$tmp/vsout0.txt" "REFUSED"
[[ ! -e $tmp/o2 ]] && ok "nothing written for a refused harvest" || bad "output exists after a refusal"
refill noprof 4 "$tmp/o2b" --package "$tmp/pkg" --harvest "$tmp/h-noprof"
expect_in "unknown profile refused" "$tmp/noprof.txt" "its profile is unknown"
refill relword 4 "$tmp/o2c" --package "$tmp/pkg" --harvest "$tmp/h-ok" --release-word 0123456789abcdef
expect_in "--release-word wins" "$tmp/relword.txt" "the release tree's is 0123456789abcdef"
# a pulled console folder with a log of another profile is refused too (when it has a word)
cp -r "$tmp/pull" "$tmp/pull-vs0"; cp "$tmp/h-vs0/run.log" "$tmp/pull-vs0/run.log"
refill pullvs0 4 "$tmp/o2d" --package "$tmp/pkg" --pulled "$tmp/pull-vs0"

# 3. closure: a record without a key recipe; a record without a compiled file
sim "$tmp/h-norecipe" --profile-tool "$tmp/pw" $relenv --no-recipe 3
refill norecipe 3 "$tmp/o3" --package "$tmp/pkg" --harvest "$tmp/h-norecipe"
expect_in "3 records without a recipe" "$tmp/norecipe.txt" "3 open: 3 without a key recipe"
expect_in "closure report written" "$tmp/o3/CLOSURE-REPORT.txt" "no key recipe in the key list"
refill norecipe-report 0 "$tmp/o3b" --package "$tmp/pkg" --harvest "$tmp/h-norecipe" --closure report
expect_in "report mode says NOT GATED" "$tmp/norecipe-report.txt" "NOT GATED"
sim "$tmp/pkg-skip" --skip 2
sim "$tmp/h-skip" --profile-tool "$tmp/pw" $relenv --skip 2
refill nofile 3 "$tmp/o3c" --package "$tmp/pkg-skip" --harvest "$tmp/h-skip"
expect_in "records without a compiled file" "$tmp/nofile.txt" "without a compiled file"
expect_in "missing files listed" "$tmp/o3c/MISSING-closure.txt" ".gxp"
# the harvest that has the two files the console seed lacks closes it (proven bytes)
refill closes 0 "$tmp/o3d" --package "$tmp/pkg-skip" --harvest "$tmp/h-ok"
expect_in "emulator files taken once proven" "$tmp/closes.txt" "compiled files of the emulator harvest taken"

# strk31: console files the emulator harvest lacks are kept and counted (CONSOLE-ONLY.txt): the package seed and the harvest both miss 2 files, the console pull has them
refill consoleonly 0 "$tmp/o3e" --package "$tmp/pkg-skip" --pulled "$tmp/pull" --harvest "$tmp/h-skip"
expect_in "console-only line" "$tmp/consoleonly.txt" "console-only files (compiled on the console, in no emulator harvest): 2 of"
nco=$(wc -l < "$tmp/o3e/CONSOLE-ONLY.txt"); [[ $nco -eq 2 ]] && ok "CONSOLE-ONLY.txt lists the 2 files" || bad "CONSOLE-ONLY.txt has $nco lines, wanted 2"
refill consoleonly0 0 "$tmp/o3f" --package "$tmp/pkg" --pulled "$tmp/pull" --harvest "$tmp/h-ok"
expect_in "no console-only file when the harvest has them all" "$tmp/consoleonly0.txt" "console-only files (compiled on the console, in no emulator harvest): 0 of"

# 4. byte compare
sim "$tmp/h-salt" --profile-tool "$tmp/pw" $relenv --fake-salt 1 --extra 6
refill broken 3 "$tmp/o4" --package "$tmp/pkg" --harvest "$tmp/h-salt"
expect_in "BROKEN" "$tmp/broken.txt" "BROKEN"
expect_in "differing names listed" "$tmp/o4/COMPILER-IDENTITY.txt" "DIFF "
expect_in "emulator files not taken" "$tmp/broken.txt" "are NOT taken"
n=$(find "$tmp/o4/shadercache" -name '*.gxp' | wc -l); np=$(find "$tmp/pkg/shadercache" -name '*.gxp' | wc -l)
[[ $n -le $np ]] && ok "no emulator file in the output ($n files, the console seed has $np)" || bad "emulator files leaked into the output ($n > $np)"
refill listsonly 0 "$tmp/o4b" --package "$tmp/pkg" --harvest "$tmp/h-salt" --harvest-files lists-only
refill unproven 3 "$tmp/o4c" --package "$tmp/pkg" --harvest "$tmp/h-ok" --min-overlap 100000
expect_in "UNPROVEN" "$tmp/unproven.txt" "UNPROVEN"

# 5. caps. A harvest whose stage list is over a cap (records with real hashes, so only the cap can fail)
python3 -I - "$root" "$tmp" <<'PY'
import importlib.util, shutil, struct, subprocess, sys
root, tmp = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("rp", root + "/tools/pack24/refill_pack.py"); rp = importlib.util.module_from_spec(spec); spec.loader.exec_module(rp)
clo = subprocess.run([tmp + "/listnames", tmp + "/list.bin", "--closure"], capture_output=True, text=True).stdout.split("\n")
v = [int(l.split()[1], 16) for l in clo if l.startswith("v ")]; f = [int(l.split()[1], 16) for l in clo if l.startswith("f ")]
def variant(name, build):
    shutil.copytree(tmp + "/h-ok", tmp + "/" + name)
    sc = tmp + "/" + name + "/shadercache"
    base = open(sc + "/vgx_stagewarm.bin", "rb").read()
    recs, ply = build()
    if recs is not None: open(sc + "/vgx_stagewarm.bin", "wb").write(base + b"".join(struct.pack("<IIQQ", *r) for r in recs))
    if ply is not None:
        body = b"".join(struct.pack("<IIQQ", rp.PLAYER_KEY, 0x16400 + i, v[i % len(v)], f[(i * 5) % len(f)]) for i in range(ply))
        h = 2166136261
        for b in body: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
        open(sc + "/vgx_stagewarm.bin.ply", "wb").write(b"VGXPLY01" + struct.pack("<II", ply, h) + body)
rec = lambda st, i: (st, 0x20000 + i, v[i % len(v)], f[(i * 7) % len(f)])
variant("h-stagecap", lambda: ([rec(0x0A0B0C0D, i) for i in range(2049)], None))
variant("h-memcap", lambda: ([rec(0x1000 + s, i) for s in range(20) for i in range(1700)], None))
variant("h-plycap", lambda: (None, 4100))
# fix31: the same draw recorded under every scenario hint, unhinted and slow-tagged, in two legs (the game keeps a draw at most twice on one card; a union of
# legs kept one copy a hint); and a can-draw copy of it. The merge keeps ONE record, unhinted, slow.
import itertools
CD, SLOW = 1 << 25, 1 << 28
hv, hf = v[3], f[5]
hints = [(0xBEEF00 + 0, 0x16400 | (k << 29), hv, hf) for k in range(8)] + [(0xBEEF00, 0x16400 | SLOW, hv, hf), (0xBEEF00, 0x16400 | CD | (2 << 29), hv, hf)]
variant("h-hintA", lambda: (hints[:5] + [rec(0xBEEF00, i) for i in range(3)], None))
variant("h-hintB", lambda: (hints[5:], None))
# can-draw records: 2,048 own + 1,024 can-draw on one stage is under every cap (seedgate31 counted the can-draw ones against the own 2,048)
cdrec = lambda st, i: (st, CD | (0x40000 + i), v[(i * 3) % len(v)], f[(i * 11) % len(f)])
variant("h-cdok", lambda: ([rec(0x0A0B0C0E, i) for i in range(2048)] + [cdrec(0x0A0B0C0E, i) for i in range(1024)], None))
variant("h-cdstage", lambda: ([cdrec(0x0A0B0C0F, i) for i in range(1025)], None))
variant("h-cdmem", lambda: ([cdrec(0x2000 + s, i) for s in range(17) for i in range(1000)], None))
# the journal: what a leg learned after the stage file's last append
def journal(name, recs, tmp=False):
    shutil.copytree(tmp_ + "/h-ok", tmp_ + "/" + name)
    body = b"".join(struct.pack("<IIQQ", *r) for r in recs)
    h = 2166136261
    for b in body: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    open(tmp_ + "/" + name + "/shadercache/vgx_stagewarm.bin.jnl" + (".tmp" if tmp else ""), "wb").write(b"VGXSTJ01" + struct.pack("<II", len(recs), h) + body)
tmp_ = tmp
journal("h-jnl", [rec(0x0C0D0E0F, i) for i in range(5)])
journal("h-jnltmp", [rec(0x0C0D0E0F, i) for i in range(6)], tmp=True)
# harvprep31: the prune by the legs' draw evidence. Stage 0a0b0c10 holds 2,100 records on the leg's card (a harvest leg keeps 8,192 a stage): 2,048 the
# package's, 52 learned. The evidence (.drawn, VGXSTD01: draw bits only) says 1,900 were drawn (every learned one among them, and one slow-tagged record by its
# draw bits); 200 package records no leg drew. fill: 148 of those kept (the first in list order), 52 dropped; all: 200 dropped.
def drawn(name, recs, tmp=False):
    body = b"".join(struct.pack("<IIQQ", *r) for r in recs)
    h = 2166136261
    for b in body: h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    open(tmp_ + "/" + name + "/shadercache/vgx_stagewarm.bin.drawn" + (".tmp" if tmp else ""), "wb").write(b"VGXSTD01" + struct.pack("<II", len(recs), h) + body)
P = 0x0A0B0C10
prune = [rec(P, i) for i in range(2100)]
prune[5] = (P, prune[5][1] | SLOW, prune[5][2], prune[5][3])   # (a slow tag: the evidence matches it by its draw bits)
undrawn = set(range(1000, 1200))
ev = [(P, r[1] & 0x1FFFFFF, r[2], r[3]) for i, r in enumerate(prune) if i not in undrawn]
variant("h-prune", lambda: (prune, None)); drawn("h-prune", ev)
variant("h-prune2", lambda: (prune[:2048], None)); drawn("h-prune2", ev[:300], tmp=True)   # (a second leg: its evidence in a complete .tmp)
variant("h-prunebig", lambda: (prune, None)); drawn("h-prunebig", [(P, r[1] & 0x1FFFFFF, r[2], r[3]) for r in prune])
variant("h-prunenoev", lambda: (prune, None))
PY
refill prune 0 "$tmp/o7a" --package "$tmp/pkg" --harvest "$tmp/h-prune" --harvest "$tmp/h-prune2"
expect_in "prune said, per stage" "$tmp/prune.txt" "prune: stage 0a0b0c10: 2100 own records, over the release's 2048: 1900 drawn by the harvest (kept; by legs that drew them: 1 leg(s) 1600, 2-3 leg(s) 300), 200 drawn by no leg: 148 kept, 52 DROPPED (--prune fill) -> 2048 records"
expect_in "draw evidence read (.drawn and a complete .drawn.tmp)" "$tmp/prune.txt" "draw evidence (harvprep31): 2 of 2 harvest folder(s) hold vgx_stagewarm.bin.drawn"
python3 -I - "$root" "$tmp/o7a" <<'PY' && ok "prune fill: every drawn record kept, the last 52 undrawn dropped, list order kept, the stage at 2,048" || bad "prune fill result wrong"
import sys
sys.path.insert(0, sys.argv[1] + "/tools/seedrun")
import stagetags as t
o = sys.argv[2]
r = [x for x in t.read(o + "/shadercache/vgx_stagewarm.bin") if x[0] == 0x0A0B0C10]
assert len(r) == 2048, len(r)
lbs = [x[1] & t.DRAW for x in r]
assert lbs == sorted(lbs), "order"
gone = {0x20000 + i for i in range(1148, 1200)}
assert not gone & set(lbs) and (0x20000 + 1147) in lbs and (0x20000 + 1200) in lbs and (0x20000 + 2099) in lbs
assert any(x[1] & t.SLOW for x in r)
d = open(o + "/PRUNED-records.txt").read().split("\n")
assert len([x for x in d if x]) == 52 and all("drawn by no harvest leg" in x for x in d if x)
rank = open(o + "/PRUNE-RANK.tsv").read().split("\n")
assert rank[1].split("\t")[1] == "2" and len([x for x in rank[1:] if x]) == 2100
PY
refill pruneall 0 "$tmp/o7b" --package "$tmp/pkg" --harvest "$tmp/h-prune" --prune all
expect_in "prune all: every undrawn dropped" "$tmp/pruneall.txt" "200 drawn by no leg: 0 kept, 200 DROPPED (--prune all) -> 1900 records"
refill prunebig 3 "$tmp/o7c" --package "$tmp/pkg" --harvest "$tmp/h-prunebig"
expect_in "prune: drawn records over the cap fail loud" "$tmp/prunebig.txt" "CAP: stage 0a0b0c10: 2100 records drawn by the harvest, more than the release's 2048 a stage"
refill prunenoev 3 "$tmp/o7d" --package "$tmp/pkg" --harvest "$tmp/h-prunenoev"
expect_in "prune: no evidence fails loud" "$tmp/prunenoev.txt" "no harvest folder holds draw evidence"
refill pruneoff 3 "$tmp/o7e" --package "$tmp/pkg" --harvest "$tmp/h-prune" --prune off
expect_in "prune off fails loud" "$tmp/pruneoff.txt" "--prune off"
refill hints 0 "$tmp/o5f" --package "$tmp/pkg" --harvest "$tmp/h-hintA" --harvest "$tmp/h-hintB"
python3 -I - "$root" "$tmp/o5f/shadercache/vgx_stagewarm.bin" <<'PY' && ok "one draw under 8 hints, slow and can-draw in two legs: one record, unhinted, slow" || bad "hinted copies of one draw not merged"
import sys
sys.path.insert(0, sys.argv[1] + "/tools/seedrun")
import stagetags as t
recs = [r for r in t.read(sys.argv[2]) if r[0] == 0xBEEF00]
same = [r for r in recs if r[1] & t.DRAW == 0x16400]
assert len(same) == 1, [hex(r[1]) for r in same]
assert same[0][1] == 0x16400 | t.SLOW, hex(same[0][1])
assert len(recs) == 4, len(recs)   # (+ the three other draws of h-hintA)
PY
expect_in "merge said" "$tmp/hints.txt" "stage-list merge (on the draw: stagetags normalize)"
refill cdok 0 "$tmp/o5g" --package "$tmp/pkg" --harvest "$tmp/h-cdok"
expect_in "can-draw counted apart" "$tmp/cdok.txt" "can-draw 1024 of 16384 (largest stage 1024 of 1024)"
refill cdstage 3 "$tmp/o5h" --package "$tmp/pkg" --harvest "$tmp/h-cdstage"
expect_in "can-draw stage cap loud" "$tmp/cdstage.txt" "CAP: stage 0a0b0c0f: 1 can-draw record(s) over the game's 1024 a stage dropped"
refill cdmem 3 "$tmp/o5i" --package "$tmp/pkg" --harvest "$tmp/h-cdmem"
expect_in "can-draw memory cap loud" "$tmp/cdmem.txt" "616 can-draw record(s) over the 16384 the game keeps in memory dropped"
refill jnl 0 "$tmp/o5j" --package "$tmp/pkg" --harvest "$tmp/h-jnl"
expect_in "journal read" "$tmp/jnl.txt" "(.jnl): 5 records, 5 new draws"
refill jnltmp 0 "$tmp/o5k" --package "$tmp/pkg" --harvest "$tmp/h-jnltmp"
expect_in "complete journal .tmp read first" "$tmp/jnltmp.txt" "(.jnl) (its complete .tmp): 6 records, 6 new draws"
# fix31: sim_pull's leg cards (what tools/pack24/sim_harvest.py runs at harvest scale): two legs from the same seed, the same stage in other scenarios,
# can-draw records and a journal; the union holds every draw once
# shellcheck disable=SC2086
sim "$tmp/leg1" --profile-tool "$tmp/pw" $relenv --no-files --base-stage "$tmp/pkg/shadercache/vgx_stagewarm.bin" --stops AstroGalaxy:1,AstroGalaxy:2 --pool-min 30 --candraw-pool 20
# shellcheck disable=SC2086
sim "$tmp/leg2" --profile-tool "$tmp/pw" $relenv --no-files --base-stage "$tmp/pkg/shadercache/vgx_stagewarm.bin" --stops AstroGalaxy:3,AstroGalaxy:1 --pool-min 30 --candraw-pool 20
[[ -f $tmp/leg1/shadercache/vgx_stagewarm.bin.jnl ]] && ok "a leg card has its journal" || bad "no journal in the leg card"
refill legs 0 "$tmp/o5l" --package "$tmp/pkg" --harvest "$tmp/leg1" --harvest "$tmp/leg2" --harvest-files lists-only
python3 -I - "$root" "$tmp/o5l/shadercache/vgx_stagewarm.bin" <<'PY' && ok "two legs' cards: every draw once, the hints that disagree unhinted" || bad "two legs' cards: a draw twice"
import sys
sys.path.insert(0, sys.argv[1] + "/tools/seedrun")
import stagetags as t
r = [x for x in t.read(sys.argv[2]) if x[0] != t.STAMP_KEY]
ids = [(x[0], x[2], x[3], x[1] & (t.DRAW | t.MENUCLS), bool(x[1] & t.CANDRAW)) for x in r]
assert len(ids) == len(set(ids)), len(ids) - len(set(ids))
assert any(x[1] & t.SCEN for x in r) and any(x[1] & t.CANDRAW for x in r)
PY
refill stagecap 3 "$tmp/o5" --package "$tmp/pkg" --harvest "$tmp/h-stagecap"
expect_in "per-stage cap loud" "$tmp/stagecap.txt" "CAP: stage 0a0b0c0d: 1 record(s) over the game's 2048 a stage dropped"
refill memcap 3 "$tmp/o5b" --package "$tmp/pkg" --harvest "$tmp/h-memcap"
expect_in "memory cap loud" "$tmp/memcap.txt" "more than the game keeps in memory (32768)"
refill plycap 3 "$tmp/o5c" --package "$tmp/pkg" --harvest "$tmp/h-plycap"
expect_in "player cap loud" "$tmp/plycap.txt" "player list over 4096 records"
refill packcap 3 "$tmp/o5d" --package "$tmp/pkg" --harvest "$tmp/h-ok" --max-entries 20
expect_in "pack cap loud" "$tmp/packcap.txt" "CAP: the merged seed is over the RAM pack's limits"
refill packusage 0 "$tmp/o5e" --package "$tmp/pkg" --harvest "$tmp/h-ok" --max-entries 20 --pack-overflow usage
expect_in "overflow by usage said" "$tmp/packusage.txt" "OVERFLOW (--pack-overflow usage)"
python3 -I - "$root" <<'PY' && ok "compact seed caps" || bad "compact seed caps"
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
import struct, c182_compact_seed as c
e, b, loose, probs = c.cap_report([100] * 8193)
assert e == 8193 and any("entries" in p for p in probs), probs
e, b, loose, probs = c.cap_report([4096] * 2049)
assert b > c.MAX_BODY and any("body bytes" in p for p in probs), probs
e, b, loose, probs = c.cap_report([100, 5000, 0])
assert (e, loose, probs) == (1, 2, []), (e, loose, probs)
pack = bytearray(40); pack[:8] = b"VGXRAM01"
def rec(n):
    p = bytearray(160 + n); p[4:8] = b"GXP\0"; return bytes(p)
files = {"shadercache/b4d4c2a9/stages.pack": bytes(pack), "shadercache/b4d4c2a9/v3/v/00/1.gxp": rec(0), "shadercache/b4d4c2a9/v3/v/00/2.gxp": rec(5000)}
c.compact(files)                     # the old behaviour: the oversized file stays loose
try: c.compact(files, strict=True); raise SystemExit("strict did not refuse an oversized file")
except ValueError as x: assert "would stay loose" in str(x)
PY

# 6. --gate-only; the verification tool
refill gateonly 0 "$tmp/o6" --package "$tmp/pkg" --gate-only
expect_in "gate-only closure" "$tmp/gateonly.txt" "0 open"
refill gateonly-bad 3 "$tmp/o6b" --package "$tmp/pkg-skip" --gate-only
refill gateonly-vs0 4 "$tmp/o6c" --package "$tmp/pkg" --gate-only --profile-log "$tmp/h-vs0/run.log"
cat > "$tmp/play.log" <<LOG
profile: word=$("$tmp/pw" print | head -1 | sed 's/.*word=\([0-9a-f]*\).*/\1/') gen=2 switches=30
GXV_PACK_MISSLOG=1 pack: on
cover: stage=099e9a42 why=left profile=0 play_s=41.0 links=3 link_ms=12 link_worst_ms=7 compiles=0 compile_ms=0 new_recs=0 new_candraw=0 uploads=2 targets=0 hold_ms=900
cover: stage=0a0b0c0d why=exit profile=0 play_s=5.0 links=0 link_ms=0 link_worst_ms=0 compiles=1 compile_ms=300 new_recs=4 new_candraw=0 uploads=0 targets=0 hold_ms=0
pack: hits 10 misses 0 compiled-in-play 0 (window 60 s: compiled 0 in all, user pack hits 0; since start: hits 10 misses 0 (0 distinct keys, 0 past the table) compiled 0, compiled-in-play 0)
LOG
python3 -I "$mk" profile "$tmp/play.log" >"$tmp/mk1.txt" 2>&1; expect_rc "misskeys profile" 0 $?
python3 -I "$mk" cover "$tmp/play.log" >"$tmp/mk2.txt" 2>&1; expect_rc "misskeys cover" 0 $?
expect_in "cover table has the stage" "$tmp/mk2.txt" "099e9a42"
word=$("$tmp/pw" print | head -1 | sed 's/.*word=\([0-9a-f]*\).*/\1/')
python3 -I "$mk" check "$tmp/play.log" --profile-word "$word" >"$tmp/mk3.txt" 2>&1; expect_rc "misskeys check profile ok" 0 $?
python3 -I "$mk" check "$tmp/play.log" --profile-word 0123456789abcdef >"$tmp/mk4.txt" 2>&1; expect_rc "misskeys check wrong word" 3 $?
python3 -I "$mk" check "$tmp/play.log" --fixed-point >"$tmp/mk5.txt" 2>&1; expect_rc "misskeys fixed point fails on compiles / new records" 3 $?
expect_in "fixed point names the stage" "$tmp/mk5.txt" "stage 0a0b0c0d: compiles=1"
# fix31: a fixed point passes on zeros; can-draw records learned fail it; a periodic line earlier in a visit counts; a line of a build before fix31 proves nothing
fp() { # name want lines...
  local name=$1 want=$2; shift 2
  { head -2 "$tmp/play.log"; printf '%s\n' "$@"; sed -n 5p "$tmp/play.log"; } > "$tmp/$name.log"
  python3 -I "$mk" check "$tmp/$name.log" --fixed-point >"$tmp/$name.txt" 2>&1; expect_rc "misskeys fixed point: $name" "$want" $?
}
z="links=0 link_ms=0 link_worst_ms=0 compiles=0 compile_ms=0"
fp fp-zero 0 "cover: stage=099e9a42 why=left profile=0 play_s=41.0 $z new_recs=0 new_candraw=0 uploads=0 targets=0 hold_ms=900"
fp fp-cd 3 "cover: stage=099e9a42 why=left profile=0 play_s=41.0 $z new_recs=0 new_candraw=7 uploads=0 targets=0 hold_ms=900"
expect_in "can-draw records learned named" "$tmp/fp-cd.txt" "stage 099e9a42: new_candraw=7 can-draw records learned"
fp fp-periodic 3 "cover: stage=099e9a42 why=periodic profile=0 play_s=60.0 $z new_recs=2 new_candraw=0 uploads=0 targets=0 hold_ms=900" \
   "cover: stage=0a0b0c0d why=left profile=0 play_s=41.0 $z new_recs=0 new_candraw=0 uploads=0 targets=0 hold_ms=900" \
   "cover: stage=099e9a42 why=exit profile=0 play_s=90.0 $z new_recs=0 new_candraw=0 uploads=0 targets=0 hold_ms=900"
expect_in "an earlier line of a stage counts" "$tmp/fp-periodic.txt" "stage 099e9a42: new_recs=2"
fp fp-old 3 "cover: stage=099e9a42 why=left profile=0 play_s=41.0 $z new_recs=0 uploads=0 targets=0 hold_ms=900"
expect_in "a line without new_candraw refused" "$tmp/fp-old.txt" "without new_candraw"
echo "cap-full: stage 0a0b0c0d holds 2048 records (limit 2048): further records of this stage are dropped" >> "$tmp/play.log"
python3 -I "$mk" check "$tmp/play.log" >"$tmp/mk6.txt" 2>&1; expect_rc "misskeys check fails on cap-full" 3 $?
expect_in "cap-full named" "$tmp/mk6.txt" "cap-full:"

echo
if [[ $fails == 0 ]]; then echo "ok: test_seedgate"; else echo "test_seedgate: $fails FAILED"; exit 1; fi

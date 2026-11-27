#!/usr/bin/env bash
# strk31: host tests of the Strikers-notes items. No game build, no console, no network.
#   1 tools/console31/check_boot_log.py : the clocks line (request, refusal 0x802B0000, missing line, missing file)
#   2 env refusals print a line: tools/check_env_file.py (empty name, no '='), make_jobs.py install (env-user.txt skipped), the game's LoadEnvFile counts both
#   3 the shader-cache pull job (make_jobs.py) is read-only: only close / sleep / ls / pull_tree
#   4 tools/release_gates.py : buildid, wformat (a real compiler on a fake database), emu, twin (PASS / FAIL / INCONCLUSIVE)
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strk31.XXXXXX"); [[ -n ${KEEP:-} ]] || trap 'rm -rf "$tmp"' EXIT
fails=0; passes=0
ok() { echo "ok:   $*"; passes=$((passes + 1)); }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
rc_is() { # name want got
  if [[ $3 == "$2" ]]; then ok "$1 (exit $3)"; else bad "$1: exit $3, wanted $2"; fi
}
has() { # name file text
  if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1: '$3' not in $2"; tail -8 "$2"; fi
}
cc=${CXX:-g++}

# ---- 1. the clocks line -------------------------------------------------------------------------------------------------------------------
cb="$root/tools/console31/check_boot_log.py"
start='clocks: CPU request=500 arm_rc=0x00000000 fallback444 fallback_tried=0 fallback_rc=0x00000000; bus request=222 bus_rc=0x00000000; GPU request=222 gpu_rc=0x00000000; xbar request=166 xbar_rc=0x00000000; reported CPU=444 reported bus=222 reported GPU=222 reported xbar=166 MHz (plugin-dependent API reports, not calibrated effective clocks)'
meas='clocks: the ARM core measures 444 MHz before the requests and 444 MHz after them (496-500: a 500 MHz profile works; 440-444: it does not)'
cat > "$tmp/l-ok.txt" <<E
$start
$meas
clocks: 1 s into the process the ARM core measures 444 MHz (asked for 500; arm_rc=0x00000000 fallback_rc=0x00000000); now 500 MHz
E
cat > "$tmp/l-ref.txt" <<E
$start
$meas
clocks: 1 s into the process the ARM core measures 444 MHz (asked for 500; arm_rc=0x802B0000 fallback_rc=0x00000000); now 444 MHz
E
cat > "$tmp/l-prof.txt" <<E
$start
$meas
clocks: 1 s into the process the ARM core measures 496 MHz (a profile is in place: no request; arm_rc=0x00000000 fallback_rc=0x00000000); now 496 MHz
E
printf 'boot\nfps: shown 30\n' > "$tmp/l-none.txt"
printf '%s\n%s\n' "$start" "$meas" > "$tmp/l-nodefer.txt"
python3 -I "$cb" "$tmp/l-ok.txt" > "$tmp/o1.txt"; rc_is "accepted 500 request logged" 0 $?; has "accepted: says rc 0" "$tmp/o1.txt" "request accepted (rc 0)"
python3 -I "$cb" "$tmp/l-ref.txt" > "$tmp/o2.txt"; rc_is "refused 500 request is a note, not a failure" 0 $?; has "refusal named" "$tmp/o2.txt" "REFUSED (rc 0x802B0000); fallback 444"
python3 -I "$cb" "$tmp/l-prof.txt" > "$tmp/o3.txt"; rc_is "profile in place" 0 $?; has "profile noted" "$tmp/o3.txt" "a profile held the core at 496 MHz"
python3 -I "$cb" "$tmp/l-none.txt" > "$tmp/o4.txt"; rc_is "no clocks line fails" 1 $?; has "missing line said" "$tmp/o4.txt" "no 'clocks:' line at all"
python3 -I "$cb" "$tmp/l-nodefer.txt" > "$tmp/o5.txt"; rc_is "deferred line missing fails" 1 $?; has "deferred line said" "$tmp/o5.txt" "no deferred-request line"
python3 -I "$cb" "$tmp/none.txt" > "$tmp/o6.txt"; rc_is "no such file" 2 $?

# ---- 2. refusals print a line -------------------------------------------------------------------------------------------------------------
printf 'GXV_A=1\n=5\nnoequals\nGXV_B=ux0:data/smg/x.bin\n' > "$tmp/env.txt"
python3 -I "$root/tools/check_env_file.py" "$tmp/env.txt" > "$tmp/e1.txt"; rc_is "empty name is an error" 1 $?
has "empty name line" "$tmp/e1.txt" "E empty variable name"
has "no '=' line" "$tmp/e1.txt" "W no '=': ignored"
printf 'GXV_STAGEWARM_FILE=ux0:data/smg/a/b.bin\nPETARI_STAGE=AstroGalaxy:1,HoneyBeeKingdomGalaxy:1\n' > "$tmp/env2.txt"
python3 -I "$root/tools/check_env_file.py" "$tmp/env2.txt" > "$tmp/e2.txt"; rc_is "a value with / and : is accepted (and listed)" 0 $?; has "listed" "$tmp/e2.txt" "2 variables, 0 errors"
# the game: both refusals counted and logged
has "game counts lines without '='" "$root/vita/vgx-game/entry.cpp" "env.txt: WARNING %d line(s) without '=' and %d line(s) with an empty or refused name were ignored"
# make_jobs: install says what it does not install
python3 -I "$root/tools/console31/make_jobs.py" --out "$tmp/c31" all > "$tmp/m1.txt" 2>&1; rc_is "make_jobs all" 0 $?
mkdir -p "$tmp/relay/staging" "$tmp/relay/inbox" "$tmp/relay/done"; echo "# none" > "$tmp/c31/staging/env-user.txt"
python3 -I "$root/tools/console31/make_jobs.py" --out "$tmp/c31" install --relay "$tmp/relay" > "$tmp/m2.txt" 2>&1; rc_is "make_jobs install" 0 $?
has "install says it skipped env-user.txt" "$tmp/m2.txt" "skipped staging/env-user.txt"
[[ ! -e $tmp/relay/staging/env-user.txt ]] && ok "env-user.txt was not installed" || bad "env-user.txt was copied to the relay"

# ---- 3. the shader-cache pull job is read-only ----------------------------------------------------------------------------------------------
job=$(ls "$tmp"/c31/jobs/*-c31-pull-shadercache.json 2>/dev/null | head -1)
if [[ -n $job ]]; then
  python3 -I - "$job" <<'PY' && ok "pull job: only close/sleep/ls/pull_tree, no put/launch/delete, pull_tree of the cache" || bad "pull job is not read-only"
import json, sys
st = json.load(open(sys.argv[1]))["steps"]
acts = [s.get("action", "sleep") for s in st]
assert set(acts) <= {"close", "sleep", "ls", "pull_tree"}, acts
pt = [s for s in st if s.get("action") == "pull_tree"]
assert len(pt) == 1 and pt[0]["path"] == "ux0:data/smg/shadercache", pt
PY
  has "pull job is in JOBS.txt" "$tmp/c31/JOBS.txt" "c31-pull-shadercache"
else bad "no pull-shadercache job"; fi
has "docs say it" "$root/docs/CONSOLE-SESSION-3.1.md" "8985-c31-pull-shadercache"

# ---- 4. release_gates.py ----------------------------------------------------------------------------------------------------------------------
rg="$root/tools/release_gates.py"
printf 'junk\0ver\0MP_BUILD_ID=%s\0more' "3.0-rc1" > "$tmp/elf.bin"; printf 'build_id 3.0-rc1\nlabel x\n' > "$tmp/info.txt"
python3 -I "$rg" buildid --elf "$tmp/elf.bin" --info "$tmp/info.txt" --expect 3.0-rc1 --release > "$tmp/g1.txt"; rc_is "buildid: release id in ELF and info" 0 $?
python3 -I "$rg" buildid --elf "$tmp/elf.bin" --expect 3.0 > "$tmp/g2.txt"; rc_is "buildid: id not in the ELF (a substring of another string is not enough)" 1 $?
{ printf 'x\0'; printf 'MP_BUILD_ID=3.1-dev\0y'; } > "$tmp/elf2.bin"
python3 -I "$rg" buildid --elf "$tmp/elf2.bin" --expect 3.1-dev --release > "$tmp/g3.txt"; rc_is "buildid: a test id is refused for a release" 1 $?
python3 -I "$rg" buildid --elf "$tmp/elf2.bin" --expect 3.1-dev > "$tmp/g3b.txt"; rc_is "buildid: the test id passes a test build" 0 $?
# the false pass of integ-3.1 (fable-review-integ31c): a 3.1-dev ELF holds the plain string 3.0.0 (SMG_APP_VERSION); it must not pass for build id 3.0.0
{ printf 'x\0'; printf '3.0.0\0MP_BUILD_ID=3.1-dev\0y'; } > "$tmp/elf3.bin"; printf 'build_id 3.0.0\n' > "$tmp/info3.txt"
python3 -I "$rg" buildid --elf "$tmp/elf3.bin" --info "$tmp/info3.txt" --expect 3.0.0 --release > "$tmp/g5.txt"; rc_is "buildid: a 3.1-dev ELF with a plain 3.0.0 string fails (the old false pass)" 1 $?
python3 -I "$rg" buildid --elf "$tmp/elf3.bin" --expect 3.0.0 > "$tmp/g5b.txt"; rc_is "buildid: ... also without --release" 1 $?
{ printf 'x\0'; printf '3.0.0\0MP_BUILD_ID=3.0.0\0y'; } > "$tmp/elf4.bin"
python3 -I "$rg" buildid --elf "$tmp/elf4.bin" --expect 3.0.0 --release > "$tmp/g6.txt"; rc_is "buildid: --release without --info is refused" 1 $?
python3 -I "$rg" buildid --elf "$tmp/elf4.bin" --info "$tmp/info3.txt" --expect 3.0.0 --release > "$tmp/g7.txt"; rc_is "buildid: a real 3.0.0 ELF with its BUILD-INFO passes" 0 $?
printf 'build_id 3.0\n' > "$tmp/info2.txt"
python3 -I "$rg" buildid --elf "$tmp/elf.bin" --info "$tmp/info2.txt" --expect 3.0-rc1 > "$tmp/g4.txt"; rc_is "buildid: BUILD-INFO says another id" 1 $?

mkdir -p "$tmp/wf"; printf '#include <cstdio>\nint good(int a) { std::printf("%%d\\n", a); return a; }\n' > "$tmp/wf/good.cpp"
printf '#include <cstdio>\nint bad(const char* s) { std::printf("%%d\\n", s); std::printf(s); return 0; }\n' > "$tmp/wf/bad.cpp"
python3 -I - "$tmp/wf" "$cc" <<'PY'
import json, sys
d, cc = sys.argv[1], sys.argv[2]
db = [{"directory": d, "file": f"{d}/{n}.cpp", "command": f"{cc} -std=c++17 -O1 -MD -MF {n}.d -o {n}.o -c {d}/{n}.cpp"} for n in ("good", "bad")]
json.dump(db, open(d + "/db.json", "w")); json.dump(db[:1], open(d + "/db-good.json", "w"))
PY
python3 -I "$rg" wformat --db "$tmp/wf/db-good.json" > "$tmp/w1.txt"; rc_is "wformat: a clean unit passes" 0 $?; has "wformat: counts" "$tmp/w1.txt" "1 units, 0 format warnings"
python3 -I "$rg" wformat --db "$tmp/wf/db.json" --jobs 2 > "$tmp/w2.txt"; rc_is "wformat: a bad format fails" 1 $?; has "wformat: names the warning" "$tmp/w2.txt" "wformat WARN"
[[ -z $(ls "$tmp/wf"/*.o "$tmp/wf"/*.d 2>/dev/null) ]] && ok "wformat wrote no object or dependency file" || bad "wformat left files"
python3 -I "$rg" wformat --db "$tmp/wf/db.json" --filter nomatch > "$tmp/w3.txt"; rc_is "wformat: no unit selected is not a pass" 2 $?

mkdir -p "$tmp/wf/shim"; printf 'inline int shim_only() { return 1; }\n' > "$tmp/wf/shim/shim_only.h"; printf '#include "shim_only.h"\nint useit() { return shim_only(); }\n' > "$tmp/wf/inc.cpp"
python3 -I - "$tmp/wf" "$cc" <<'PY'
import json, sys
d, cc = sys.argv[1], sys.argv[2]
json.dump([{"directory": d, "file": d + "/inc.cpp", "command": f"{cc} -std=c++17 -o inc.o -c {d}/inc.cpp"}], open(d + "/db-inc.json", "w"))
PY
python3 -I "$rg" wformat --db "$tmp/wf/db-inc.json" > "$tmp/w4.txt"; rc_is "wformat: a header found only through CPATH: the gate is not a pass" 2 $?
python3 -I "$rg" wformat --db "$tmp/wf/db-inc.json" --cpath "$tmp/wf/shim" > "$tmp/w5.txt"; rc_is "wformat: --cpath finds it" 0 $?

mkl() { # name exit faults inplay firstuse
  { echo "boot"; [[ $4 == 1 ]] && echo "stutter: a program linked by a draw in play"; [[ $5 != - ]] && echo "first-use: stage abcd1234 left after 900 turns; programs linked by draws $5";
    [[ $3 == 1 ]] && echo "FATAL: abort()"; echo "end"; } > "$tmp/$1.log"
}
mkl s1 0 0 0 0; mkl s2 0 0 0 0; mkl s3 0 0 0 0
python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s2.log" --stage c:0:"$tmp/s3.log" > "$tmp/x1.txt"; rc_is "emu: three clean stages" 0 $?
python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s2.log" > "$tmp/x2.txt"; rc_is "emu: two stages are not three" 2 $?
python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:139:"$tmp/s2.log" --stage c:0:"$tmp/s3.log" > "$tmp/x3.txt"; rc_is "emu: exit code 139" 1 $?; has "emu: exit named" "$tmp/x3.txt" "exit code 139"
mkl s4 0 1 0 0; python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s4.log" --stage c:0:"$tmp/s3.log" > "$tmp/x4.txt"; rc_is "emu: a fault line" 1 $?
mkl s5 0 0 1 0; python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s5.log" --stage c:0:"$tmp/s3.log" > "$tmp/x5.txt"; rc_is "emu: a program linked in play" 1 $?
mkl s6 0 0 0 3; python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s6.log" --stage c:0:"$tmp/s3.log" > "$tmp/x6.txt"; rc_is "emu: first-use programs linked by draws" 1 $?
mkl s7 0 0 0 -; python3 -I "$rg" emu --stage a:0:"$tmp/s1.log" --stage b:0:"$tmp/s7.log" --stage c:0:"$tmp/s3.log" > "$tmp/x7.txt"; rc_is "emu: a log that never warmed up is INVALID" 2 $?; has "emu: invalid said" "$tmp/x7.txt" "INVALID"

hl() { # file mutation-turn: 40 hashes every 300 turns; from turn $2 on the hash differs (0 = never)
  python3 - "$1" "${2:-0}" <<'PY'
import sys
out, cut = sys.argv[1], int(sys.argv[2])
with open(out, "w") as f:
    for i in range(1, 41):
        t = i * 300
        h = (0x1234567800000000 + i) if not cut or t < cut else (0x9999999900000000 + i + cut)
        f.write("undrawn-lean: hash turn %u %016x actors 120\n" % (t, h))
PY
}
hl "$tmp/t-plain.log" 0; hl "$tmp/t-plain2.log" 0; hl "$tmp/t-pgo-same.log" 0; hl "$tmp/t-pgo-bad.log" 6000; hl "$tmp/t-plain2-bad.log" 3000; hl "$tmp/t-plain2-late.log" 9000
printf 'undrawn-lean: hash turn 300 %016x actors 9\n' 1 > "$tmp/t-short.log"
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2.log" --pgo "$tmp/t-pgo-same.log" > "$tmp/y1.txt"; rc_is "twin: pgo equals plain" 0 $?
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2.log" --pgo "$tmp/t-pgo-bad.log" > "$tmp/y2.txt"; rc_is "twin: pgo leaves a reproducible plain" 1 $?
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2-bad.log" --pgo "$tmp/t-pgo-bad.log" > "$tmp/y3.txt"; rc_is "twin: plain differs from plain2 first: INCONCLUSIVE" 2 $?; has "twin: says DET INCONCLUSIVE" "$tmp/y3.txt" "DET INCONCLUSIVE"
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2-late.log" --pgo "$tmp/t-pgo-bad.log" > "$tmp/y4.txt"; rc_is "twin: plain2 leaves plain only after pgo did: FAIL" 1 $?
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2.log" --pgo "$tmp/t-short.log" > "$tmp/y5.txt"; rc_is "twin: too few hashes" 2 $?
python3 -I "$rg" twin --plain "$tmp/t-plain.log" --plain2 "$tmp/t-plain2-bad.log" --pgo "$tmp/t-pgo-same.log" > "$tmp/y6.txt"; rc_is "twin: pgo equals plain though plain2 drifts: PASS" 0 $?

echo "strk31 tests: $passes passed, $fails failed"
[[ $fails == 0 ]]

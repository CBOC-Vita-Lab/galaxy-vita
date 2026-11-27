#!/usr/bin/env bash
# GXV_NOOP_FILTER (vgx_state.cpp NoopRun: the top-level stream's BP / CP / XF repeats in one loop) and the cost of a register write by class.
#  1. tests/vgx_noop_bench.cpp: streams of one class (and a random mix) through the real vgx::ProcessStream. The register file after every
#     stream (a hash, FEWB_VERIFY=1) must be the same with GXV_NOOP_FILTER=0 / 1 / 2, and =2 (the normal path runs, and every command the loop
#     would skip is checked: dirty flags, keys, registers) must say "0 mismatching" after more than 1000 checks.
#  2. tests/vgx_fe_work.cpp (the J3D-shaped scene through the real front end): the command buffers frame for frame the same with the filter
#     off / on / verify, around GXV_SPLIT=0/1/2, GXV_FE_SPLIT=2 (the top-state-run slot), GXV_RETAINED, GXV_BPFAST=0 / 1, GXV_REGSHADOW=0, GXV_DLCACHE=0;
#     the verify's "noop filter (GXV_NOOP_FILTER=2): N checked, 0 mismatching".
#  3. negative control (-DGXV_NOOP_MUTANT=1: an XF matrix load counts as a repeat when its first word is the register's): the bench's register
#     hash must DIFFER from the unfiltered one, and the verify mode must find mismatches.
#   tools/test_vgx_noopfilter.sh [frames] [seeds]     (CXX: host compiler; objects in $OUT, default /tmp/vgx_noopf; JOBS=2)
# The cost table: tools/test_vgx_noopfilter.sh prints the bench's ns per command with the filter off and on at the end (host time; the
# instruction counts under valgrind --tool=callgrind --callgrind-out-file=x ./bench 100000 1 are load-independent).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-40}
seeds=${2:-"1 2"}
out=${OUT:-/tmp/vgx_noopf}
cxx=${CXX:-g++}
jobs=${JOBS:-2}
mkdir -p "$out/obj"
cd "$root"
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp
      tests/vgx_noop_bench.cpp)
common="-std=c++17 -O2 -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread"
hdr_newer() { [ -n "$(find platform/vgx/src tests -maxdepth 1 \( -name '*.h' -o -name '*.inc' \) -newer "$1" -print -quit)" ]; }
need() { [ ! -f "$2" ] || [ "$1" -nt "$2" ] || hdr_newer "$2"; }
for f in "${srcs[@]}"; do
  o="$out/obj/$(basename "$f" .cpp).o"
  if need "$f" "$o"; then echo "$cxx $common -c $f -o $o"; fi
done | xargs -P "$jobs" -I{} sh -c "{}"
o="$out/obj/vgx_state.mut1.o"
if need platform/vgx/src/vgx_state.cpp "$o"; then $cxx $common -DGXV_NOOP_MUTANT=1 -c platform/vgx/src/vgx_state.cpp -o "$o"; fi
lib=$(ls "$out"/obj/*.o | grep -v '\.mut1\.o' | grep -v 'vgx_fe_work.o\|vgx_noop_bench.o')
$cxx -pthread "$out/obj/vgx_fe_work.o" $lib -o "$out/few"
$cxx -pthread "$out/obj/vgx_noop_bench.o" $lib -o "$out/bench"
$cxx -pthread "$out/obj/vgx_noop_bench.o" $(echo "$lib" | grep -v 'vgx_state.o') "$out/obj/vgx_state.mut1.o" -o "$out/bench-mut"
fail=0
# --- 1. the bench
bh() { env FEWB_NOHDR=1 FEWB_VERIFY=1 "$@" | grep -v '^noopfilter\|^verify: noopfilter'; }
n=${BENCH_N:-2048}
bh GXV_NOOP_FILTER=0 "$out/bench" $n 3 > "$out/b0.txt"
bh GXV_NOOP_FILTER=1 "$out/bench" $n 3 > "$out/b1.txt"
bh GXV_NOOP_FILTER=2 "$out/bench" $n 3 > "$out/b2.txt"
# (the hash is the last field: the ns column differs between runs)
hashes() { sed 's/.*state hash //' "$1"; }
for k in 1 2; do
  if ! diff <(hashes "$out/b0.txt") <(hashes "$out/b$k.txt") > /dev/null; then echo "FAIL: bench register hashes differ with GXV_NOOP_FILTER=$k"; fail=1; fi
done
vline=$(env FEWB_NOHDR=1 FEWB_VERIFY=1 GXV_NOOP_FILTER=2 "$out/bench" $n 3 | grep '^noopfilter')
echo "bench verify: $vline"
vsum=$(env FEWB_NOHDR=1 FEWB_VERIFY=1 GXV_NOOP_FILTER=2 "$out/bench" $n 3 | grep '^verify: noopfilter: GXV_NOOP_FILTER requested 2, effective mode 2' || true)  # (verifyfix31: the game-log line)
[ -n "$vsum" ] || { echo "FAIL: no 'verify: noopfilter:' game-log line from port_vgx_verify_summary"; exit 1; }
echo "game-log line: $vsum"
set -- $(echo "$vline" | sed -n 's/noopfilter: \([0-9]*\) checked, \([0-9]*\) mismatching/\1 \2/p'); chk=${1:-0}; bad=${2:-1}
if [ "$chk" -lt 1000 ] || [ "$bad" != 0 ]; then echo "FAIL: bench verify checked $chk, mismatching $bad"; fail=1; fi
# (the filter is on: the repeats are skipped, so the unfiltered and filtered runs have the same hash but the counts of the filter are the point)
bh GXV_NOOP_FILTER=1 "$out/bench-mut" $n 3 > "$out/bm1.txt"
if diff <(hashes "$out/b0.txt") <(hashes "$out/bm1.txt") > /dev/null; then echo "FAIL: negative control (MUTANT=1) left the register file as it was: the test is blind"; fail=1; else echo "bench negative control (MUTANT=1, filter on): register hashes differ, as they must"; fi
mline=$(env FEWB_NOHDR=1 FEWB_VERIFY=1 GXV_NOOP_FILTER=2 "$out/bench-mut" $n 3 | grep '^noopfilter')
set -- $(echo "$mline" | sed -n 's/noopfilter: \([0-9]*\) checked, \([0-9]*\) mismatching/\1 \2/p'); mbad=${2:-0}
if [ "$mbad" = 0 ]; then echo "FAIL: negative control (MUTANT=1) not caught by the verify mode ($mline)"; fail=1; else echo "bench negative control (MUTANT=1, verify): $mline"; fi
# --- 2. the fixture
for seed in $seeds; do
  for scene in "FEW_CONSOLE=1 FEW_J3DFMT=1" "FEW_INST=6" "FEW_PLANTS=6 FEW_FUR=3" "FEW_CONSOLE=1 FEW_JPA=60 FEW_FXMERGE=1" "FEW_LIGHT=3 FEW_FOG=2" "FEW_J3N=1"; do
    for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_DLCACHE=0" "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2 GXV_FE_CENSUS=1" "GXV_STREAM_CENSUS=1 FEW_CENSUS=2"; do
      env $scene $around FEW_RETAINED=1 GXV_RETAINED=1 GXV_NOOP_FILTER=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
      for mode in 1 2; do
        env $scene $around FEW_RETAINED=1 GXV_RETAINED=1 GXV_NOOP_FILTER=$mode "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
        if ! cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL seed $seed: $scene $around GXV_NOOP_FILTER=$mode differs from =0"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1; fi
        if [ $mode = 2 ]; then
          if grep -q "noop filter.*: [0-9]* checked, [1-9][0-9]* mismatching" "$out/run.err" || grep -q "noopfilter: MISMATCH" "$out/run.err"; then echo "FAIL seed $seed: $scene $around: $(grep -m1 'noop filter\|noopfilter' "$out/run.err")"; fail=1; fi
        fi
      done
    done
  done
  env FEW_CONSOLE=1 FEW_J3DFMT=1 FEW_RETAINED=1 GXV_RETAINED=1 GXV_NOOP_FILTER=2 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/ver.err"
  vl=$(grep "noop filter" "$out/ver.err")
  echo "seed $seed: fixture verify: $vl"
  set -- $(echo "$vl" | sed -n 's/.*: \([0-9]*\) checked, \([0-9]*\) mismatching/\1 \2/p'); c2=${1:-0}; b2=${2:-1}
  if [ "$c2" -lt 1000 ] || [ "$b2" != 0 ]; then echo "FAIL seed $seed: fixture verify checked $c2, mismatching $b2"; fail=1; fi
done
# --- the cost table (host ns per command)
echo "ns per command (host, best of 300 runs of 4096; GXV_NOOP_FILTER=0 / 1):"
paste <(env GXV_NOOP_FILTER=0 "$out/bench" 4096 300) <(env GXV_NOOP_FILTER=1 "$out/bench" 4096 300 | awk '{print $NF}')
if [ $fail = 0 ]; then echo "PASS: GXV_NOOP_FILTER"; else exit 1; fi

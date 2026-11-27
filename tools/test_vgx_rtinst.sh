#!/usr/bin/env bash
# GXV_RT_INST (vgx_dlcache.cpp "retained shapes": records of a shape for each instance's own vertex state, and of a variant of its list other than
# the first) on the host: the J3D-shaped scene of tests/vgx_fe_work.cpp through the real front end, with FEW_INST=<n> instances a picture of ONE model
# (one geometry list, one handle, arrays of their own; every 5th picture one instance's positions are written again with other values, every 11th
# a write that stores what was there), against the same streams with GXV_RT_INST=0.
# Passes when, for each setting around it (GXV_SPLIT=0/1/2, the other scene parts, fur, plants, GXV_DRAWREPLAY=0, GXV_DLCACHE=0, GXV_STATICBUF=0,
# GXV_FE_KNOBS=0, the FE breakdown and census), the command buffers are the same frame for frame with GXV_RT_INST=0 / 1 / 2 (=2 is the verify mode:
# every call goes the normal way and a record that would have been a hit must be a hit of the same entry, "mismatching" must be 0 and no vertex or
# index byte may differ); the instance records are used (hits through them, and the retained hit share of the instances' calls rises against
# GXV_RT_INST=0, where the instances ping-pong in the model's one record); and the two negative controls behave:
#   -DGXV_RT_INST_MUTANT=2 (every instance of a shape in one slot: the tests that make a record valid must still find the other instances' records
#                           wrong: the output must be the same as the baseline: a stress of the collision case),
#   -DGXV_RT_INST_MUTANT=1 (and the vertex-state test left out for instance slots: the output MUST differ from the baseline, or this test is blind).
#   tools/test_vgx_rtinst.sh [frames] [seeds]      (CXX: host compiler, default g++; objects and binaries in $OUT, default /tmp/vgx_rtinst; JOBS=2)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_rtinst}
cxx=${CXX:-g++}
jobs=${JOBS:-2}
mkdir -p "$out/obj"
cd "$root"
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
common="-std=c++17 -O2 -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread"
# (objects are rebuilt when their source or any header is newer; the dlcache is built three times, plain and as the two mutants)
hdr_newer() { [ -n "$(find platform/vgx/src tests -maxdepth 1 \( -name '*.h' -o -name '*.inc' \) -newer "$1" -print -quit)" ]; }
need() { [ ! -f "$2" ] || [ "$1" -nt "$2" ] || hdr_newer "$2"; }
for f in "${srcs[@]}"; do
  o="$out/obj/$(basename "$f" .cpp).o"
  if need "$f" "$o"; then echo "$cxx $common -c $f -o $o"; fi
done | xargs -P "$jobs" -I{} sh -c "{}"
for m in 1 2; do
  o="$out/obj/vgx_dlcache.mut$m.o"
  if need platform/vgx/src/vgx_dlcache.cpp "$o"; then $cxx $common -DGXV_RT_INST_MUTANT=$m -c platform/vgx/src/vgx_dlcache.cpp -o "$o"; fi
done
plain=$(ls "$out"/obj/*.o | grep -v '\.mut[12]\.o')
$cxx -pthread $plain -o "$out/few"
for m in 1 2; do
  $cxx -pthread $(echo "$plain" | grep -v 'vgx_dlcache.o') "$out/obj/vgx_dlcache.mut$m.o" -o "$out/few-mut$m"
done
fail=0
cfgs=("GXV_RT_INST=1 GXV_RETAINED=1" "GXV_RT_INST=2 GXV_RETAINED=1" "GXV_RT_INST=1 GXV_RETAINED=2" "GXV_RT_INST=2 GXV_RETAINED=2")
for seed in $seeds; do
  for scene in "FEW_INST=6" "FEW_INST=9 FEW_ODD=0" "FEW_INST=5 FEW_PLANTS=6 FEW_FUR=3" "FEW_INST=4 FEW_CONSOLE=1 FEW_JPA=60" "FEW_INST=3"; do
    for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_DRAWREPLAY=0" "GXV_DLCACHE=0" "GXV_STATICBUF=0" "GXV_FE_KNOBS=0" "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2 GXV_FE_CENSUS=1"; do
      env $scene $around FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
      for cfg in "${cfgs[@]}"; do
        env $scene $around FEW_RETAINED=1 $cfg "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
        if ! cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL seed $seed: $scene $around $cfg differs from GXV_RT_INST=0"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1; fi
        if grep -q "retained shapes.*mismatching [1-9]" "$out/run.err" || grep -q "retained: MISMATCH" "$out/run.err"; then echo "FAIL seed $seed: $scene $around $cfg: $(grep -m1 'retained shapes\|MISMATCH' "$out/run.err")"; fail=1; fi
        if grep -q "vertex bytes differing [1-9]" "$out/run.err"; then echo "FAIL seed $seed: $scene $around $cfg: $(grep -m1 'vertex bytes differing' "$out/run.err")"; fail=1; fi
      done
    done
  done
  # (the instance records are used: hits through them; verify mode checks them, and finds no difference)
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=0 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/off.err"
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=1 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/on.err"
  set -- $(sed -n 's/.*retained shapes (GXV_RETAINED): \([0-9]*\) calls, \([0-9]*\) hits.*/\1 \2/p' "$out/off.err"); offc=${1:-0}; offh=${2:-0}
  set -- $(sed -n 's/.*retained shapes (GXV_RETAINED): \([0-9]*\) calls, \([0-9]*\) hits.*/\1 \2/p' "$out/on.err"); onc=${1:-0}; onh=${2:-0}
  inst=$(sed -n 's/.*instance slots used \([0-9]*\), hits \([0-9]*\), records \([0-9]*\).*/\2/p' "$out/on.err")
  echo "seed $seed: retained hits off $offh of $offc, on $onh of $onc, through instance slots ${inst:-0}"
  if [ "${onc:-0}" -lt 500 ] || [ $(( onh * 10 )) -lt $(( onc * 8 )) ]; then echo "FAIL seed $seed: with GXV_RT_INST=1 the retained hits are ${onh} of ${onc} (want >= 80 %)"; fail=1; fi
  if [ "${onh:-0}" -le "${offh:-0}" ]; then echo "FAIL seed $seed: GXV_RT_INST=1 gains no retained hits (${onh} against ${offh})"; fail=1; fi
  if [ "${inst:-0}" -lt 100 ]; then echo "FAIL seed $seed: no hits through instance slots"; fail=1; fi
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=2 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/ver.err"
  set -- $(sed -n 's/.*retained shapes (GXV_RETAINED): .*checked \([0-9]*\), mismatching \([0-9]*\)$/\1 \2/p' "$out/ver.err"); chk=${1:-0}; bad=${2:-1}
  echo "seed $seed: verify (GXV_RT_INST=2) checked $chk, mismatching $bad"
  if [ "$chk" -lt 500 ] || [ "$bad" != 0 ]; then echo "FAIL seed $seed: verify checked $chk, mismatching $bad"; fail=1; fi
  # (negative controls)
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=1 "$out/few-mut2" "$frames" "$seed" > "$out/m2.txt" 2> /dev/null
  if ! cmp -s "$out/base.txt" "$out/m2.txt"; then echo "FAIL seed $seed: the collision stress (MUTANT=2) changed the output"; fail=1; fi
  env FEW_INST=6 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_RT_INST=1 "$out/few-mut1" "$frames" "$seed" > "$out/m1.txt" 2> /dev/null
  if cmp -s "$out/base.txt" "$out/m1.txt"; then echo "FAIL seed $seed: the negative control (MUTANT=1) did not change the output: the test is blind"; fail=1; else echo "seed $seed: negative control (MUTANT=1) detected"; fi
done
if [ $fail = 0 ]; then echo "PASS: GXV_RT_INST"; else exit 1; fi

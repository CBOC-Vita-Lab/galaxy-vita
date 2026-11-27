#!/usr/bin/env bash
# GXV_J3DNATIVE (vgx_j3native.inc): the J3D-shaped scene of tests/vgx_fe_work.cpp with the game's material-list brackets (FEW_J3N=1) through
# the real front end. Passes when, for every workload, the command buffers are the same frame for frame with GXV_J3DNATIVE=0 / 1 / 2 / 3 / 4 / 5
# (also with the bracket-less stream, the same as the base), modes 2 and 3 report 0 mismatches, a scene change / light / fog / show-hide /
# animated-material workload is exact, the mutation (FEW_J3N_MISS=1: a writer that forgets its version bump) IS caught (mode 2 verify and
# mode 3's scrub / cross-check report it), and an AddressSanitizer / UndefinedBehaviorSanitizer build and a ThreadSanitizer build
# (split, two cores) report nothing. Stage 2 (=4 verify, =5 on): every patch verified in mode 4 (0 mismatching), chains engaged, the
# moving camera (FEW_CAM), an EFB copy mid-run (FEW_COPY), non-bracket lists and masks mid-run (FEW_ODD=1), retained-handle mutations
# (FEW_RETAINED=2), and the missed version bump caught by stage 2's byte comparison with the pictures exact.
#   tools/test_vgx_j3native.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_j3native)
# Instruction / cache counts: see the REPORT (callgrind --cache-sim=yes with FEW_PROFILE=1 FEW_TRASH=4096, GXV_J3DNATIVE=0 vs 3).
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
mframes=${MFRAMES:-600}  # (the mutation test: a material rewrite every ~50 pictures)
seeds=${2:-"1 2"}
out=${OUT:-$MP_TMP/vgx_j3native}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp
      platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/few"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/few-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/few-tsan"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
vita="GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 FEW_ODD=0 FEW_RETAINED=1 FEW_JPA=150 FEW_FX=60 FEW_FUR=10 FEW_PLANTS=40"
fail=0
frames_of() { grep -E '^frame [0-9]+:' "$1" || true; }
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_SPLIT=1 GXV_FE_TWOCORE=1" "$vita" "$vita GXV_FE_TWOCORE=1" "$vita GXV_SCENEDROP=2" "$vita GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    for work in "FEW_J3N=1 FEW_DIFFKEEP=1" "FEW_J3N=1 FEW_DIFFKEEP=0" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_LIGHT=7" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_FOG=5" \
                "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_LIGHT=3 FEW_FOG=11" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_SCENE=25" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_SCENE=25 FEW_SCENE_REUSE=1" "FEW_J3N=0" \
                "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_CAM=1" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_COPY=4" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_ODD=1" \
                "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_RETAINED=2 FEW_CAM=1 FEW_LIGHT=5" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_KONST=1 FEW_ODD=1 FEW_FOG=6 FEW_COPY=3"; do
      env $around $work GXV_J3DNATIVE=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
      for m in 1 2 3 4 5; do
        env $around $work GXV_J3DNATIVE=$m GXV_J3DNATIVE_VBUDGET=64 "$out/few" "$frames" "$seed" > "$out/t.txt" 2> "$out/t.err"
        if ! diff <(frames_of "$out/base.txt") <(frames_of "$out/t.txt") > /dev/null; then echo "FAIL seed $seed [$around] [$work] mode $m: frames differ"; fail=1; fi
        if [ "$m" -ge 2 ] && grep -q 'mismatching [1-9]' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode $m: mismatch reported"; grep 'fixture: j3n' "$out/t.err" | cut -c1-400; fail=1; fi
        if [ "$m" -ge 4 ] && [[ "$work" == *FEW_J3N=1* ]] && [[ "$around" != *TWOCORE* ]] && [[ "$around" == *GXV_RETAINED=1* ]] && ! grep -q 'stage 2 (GXV_J3DNATIVE=4 / 5): chains started [1-9]' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode $m: no stage-2 chains (the switch did not engage)"; fail=1; fi
        if [ "$m" = 4 ] && [[ "$work" == *FEW_J3N=1* ]] && [[ "$around" != *TWOCORE* ]] && [[ "$around" == *GXV_RETAINED=1* ]] && ! grep -q 'patches [1-9][0-9]* mismatching 0' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode 4: no patch verified"; fail=1; fi
        if [ "$m" -ge 4 ] && grep -q 'missed writers [1-9]' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode $m: stage 2 found a missed writer"; fail=1; fi
        if [ "$m" -ge 2 ] && [ "$m" -le 3 ] && [[ "$work" == *FEW_J3N=1* ]] && ! grep -q 'stage 1: [1-9][0-9]* seen' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode $m: no brackets seen (the switch did not engage)"; fail=1; fi
        if [ "$m" -ge 2 ] && grep -q 'scrubs [0-9]* (bad [1-9]' "$out/t.err"; then echo "FAIL seed $seed [$around] [$work] mode $m: scrub found a missed writer"; fail=1; fi
      done
    done
  done
done
# the mutation: a writer that forgets its version bump must be caught (the verify in mode 2, the scrub in mode 3), and the pictures stay
# exact in mode 2 (verify only) -- mode 3 may show the stale picture for up to the scrub period, which is what the test measures
for seed in $seeds; do
  env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_J3N_MISS=1 GXV_J3DNATIVE=0 "$out/few" "$mframes" "$seed" > "$out/mbase.txt" 2> /dev/null
  for m in 2 3; do
    env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_J3N_MISS=1 GXV_J3DNATIVE=$m GXV_J3DNATIVE_VBUDGET=64 GXV_J3DNATIVE_SCRUB=8 "$out/few" "$mframes" "$seed" > "$out/t.txt" 2> "$out/t.err"
    if grep -q 'mismatching [1-9]\|scrubs [0-9]* (bad [1-9]' "$out/t.err"; then echo "mutation caught (seed $seed mode $m)"; else echo "FAIL seed $seed mode $m: the missed version bump was NOT caught"; fail=1; fi
    if [ "$m" = 2 ] && ! diff <(frames_of "$out/mbase.txt") <(frames_of "$out/t.txt") > /dev/null; then echo "FAIL seed $seed: verify mode (2) changed a picture under the mutation"; fail=1; fi
  done
done
# stage 2's other switches (templates GXV_J3DNATIVE_TPL=1, no history seeding, no re-recording, no lookahead): exact, 0 mismatching
for seed in $seeds; do
  for knobs in "GXV_J3DNATIVE_TPL=1" "GXV_J3DNATIVE_HSEED=0" "GXV_J3DNATIVE_RESUME=0" "GXV_J3DNATIVE_LOOK=0" "GXV_J3DNATIVE_TRUST=1"; do
    for work in "FEW_J3N=1 FEW_DIFFKEEP=1" "FEW_J3N=1 FEW_DIFFKEEP=1 FEW_CAM=1 FEW_LIGHT=3 FEW_FOG=11 FEW_COPY=4" "FEW_J3N=1 FEW_DIFFKEEP=0 FEW_SCENE=25 FEW_SCENE_REUSE=1"; do
      env $vita $work GXV_J3DNATIVE=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
      for m in 4 5; do
        env $vita $work $knobs GXV_J3DNATIVE=$m "$out/few" "$frames" "$seed" > "$out/t.txt" 2> "$out/t.err"
        if ! diff <(frames_of "$out/base.txt") <(frames_of "$out/t.txt") > /dev/null; then echo "FAIL seed $seed [$knobs] [$work] mode $m: frames differ"; fail=1; fi
        if grep -q 'mismatching [1-9]' "$out/t.err"; then echo "FAIL seed $seed [$knobs] [$work] mode $m: mismatch reported"; fail=1; fi
      done
    done
  done
done
# stage 2 under the mutation: the tier-B byte comparison (GXV_J3DNATIVE_TRUST=0, the default) catches the missed bump and the pictures
# stay exact in =4 and =5; with TRUST=1 the scrub catches it (the picture may be stale up to the scrub period)
for seed in $seeds; do
  env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_J3N_MISS=1 GXV_J3DNATIVE=0 "$out/few" "$mframes" "$seed" > "$out/mbase.txt" 2> /dev/null
  for m in 4 5; do
    env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_J3N_MISS=1 GXV_J3DNATIVE=$m "$out/few" "$mframes" "$seed" > "$out/t.txt" 2> "$out/t.err"
    if grep -q 'missed writers [1-9]' "$out/t.err"; then echo "stage-2 mutation caught (seed $seed mode $m)"; else echo "FAIL seed $seed mode $m: the missed version bump was NOT caught by stage 2"; fail=1; fi
    if ! diff <(frames_of "$out/mbase.txt") <(frames_of "$out/t.txt") > /dev/null; then echo "FAIL seed $seed mode $m: a picture changed under the mutation"; fail=1; fi
  done
  env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_J3N_MISS=1 GXV_J3DNATIVE=5 GXV_J3DNATIVE_TRUST=1 GXV_J3DNATIVE_SCRUB=8 "$out/few" "$mframes" "$seed" > "$out/t.txt" 2> "$out/t.err"
  if grep -q 'missed writers [1-9]' "$out/t.err"; then echo "stage-2 mutation caught with TRUST=1 (seed $seed)"; else echo "FAIL seed $seed: TRUST=1 did not catch the missed bump"; fail=1; fi
done
# sanitizers
for san in few-asan few-tsan; do
  for m in 2 3 4 5; do
    env $vita GXV_FE_TWOCORE=1 FEW_J3N=1 FEW_DIFFKEEP=1 FEW_LIGHT=7 FEW_FOG=5 FEW_SCENE=25 GXV_J3DNATIVE=$m GXV_J3DNATIVE_VBUDGET=64 "$out/$san" 40 1 > "$out/s.txt" 2> "$out/s.err" || { echo "FAIL $san mode $m"; tail -20 "$out/s.err"; fail=1; }
    if grep -q 'ERROR: \(Address\|Thread\)Sanitizer\|runtime error' "$out/s.err"; then echo "FAIL $san mode $m: sanitizer report"; fail=1; fi
  done
  for m in 4 5; do # (stage 2 is the player's only, single core: the chains engaged)
    env $vita FEW_J3N=1 FEW_DIFFKEEP=1 FEW_LIGHT=7 FEW_FOG=5 FEW_SCENE=25 FEW_CAM=1 FEW_COPY=4 FEW_ODD=1 GXV_J3DNATIVE=$m "$out/$san" 120 1 > "$out/s.txt" 2> "$out/s.err" || { echo "FAIL $san mode $m (single core)"; tail -20 "$out/s.err"; fail=1; }
    if grep -q 'ERROR: \(Address\|Thread\)Sanitizer\|runtime error' "$out/s.err"; then echo "FAIL $san mode $m (single core): sanitizer report"; fail=1; fi
  done
done
[ $fail = 0 ] && echo "test_vgx_j3native: PASS" || { echo "test_vgx_j3native: FAIL"; exit 1; }

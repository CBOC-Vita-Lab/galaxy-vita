#!/usr/bin/env bash
# Front-end split (platform/vgx/src/vgx_fesplit.h) on the host: the synthetic workload of tests/vgx_fesplit_stress.cpp through
# the real front end with GXV_SPLIT=0, 1 and 2. Passes when the command buffers of 0 and 1 hash the same frame for frame,
# when 2 reports no mismatch, and when a ThreadSanitizer build of 1 and 2 reports no race.
#   tools/test_vgx_fesplit.sh [frames] [seeds]      (host clang and gcc; out-of-tree build in $OUT, default /tmp/vgx_fesplit)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-300}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_fesplit}
mkdir -p "$out"
cxx=${CXX:-clang++}
srcs=(tests/vgx_fesplit_stress.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/stress"
# (ThreadSanitizer: gcc's runtime; TSAN_CXX to choose another compiler)
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/stress-tsan"
fail=0
for seed in $seeds; do
  rm -f "$out"/list.*.$seed
  for mode in 0 1 2; do
    FESPLIT_LIST="$out/list.$mode.$seed" GXV_SPLIT=$mode "$out/stress" "$frames" "$seed" > "$out/s$mode.$seed.txt" 2> "$out/s$mode.$seed.err"
  done
  if ! cmp -s "$out/list.0.$seed" "$out/list.1.$seed"; then echo "FAIL seed $seed: shader warm-up lists differ"; fail=1; fi
  # (the finishing thread made slow at random: the player overtakes it at every point of the hand-over)
  for mode in 1 2; do
    GXV_SPLIT_JITTER=1 GXV_SPLIT=$mode "$out/stress" "$frames" "$seed" > "$out/j$mode.$seed.txt" 2> "$out/j$mode.$seed.err"
    if ! cmp -s "$out/s0.$seed.txt" "$out/j$mode.$seed.txt"; then echo "FAIL seed $seed: GXV_SPLIT=$mode with jitter differs from 0"; fail=1; fi
    if grep -q "MISMATCH\|unknown operation" "$out/j$mode.$seed.err"; then echo "FAIL seed $seed: jitter, GXV_SPLIT=$mode: $(grep -m1 'MISMATCH\|unknown operation' "$out/j$mode.$seed.err")"; fail=1; fi
  done
  if ! cmp -s "$out/s0.$seed.txt" "$out/s1.$seed.txt"; then echo "FAIL seed $seed: GXV_SPLIT=1 differs from 0"; diff "$out/s0.$seed.txt" "$out/s1.$seed.txt" | head -5; fail=1; fi
  if ! cmp -s "$out/s0.$seed.txt" "$out/s2.$seed.txt"; then echo "FAIL seed $seed: GXV_SPLIT=2 differs from 0"; fail=1; fi
  if grep -q "MISMATCH" "$out/s2.$seed.err"; then echo "FAIL seed $seed: verify mismatches"; grep MISMATCH "$out/s2.$seed.err" | head -5; fail=1; fi
  grep "fesplit: verify" "$out/s2.$seed.err" | tail -1
  for mode in 1 2; do
    if ! TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" GXV_SPLIT_JITTER=1 GXV_SPLIT=$mode "$out/stress-tsan" "$((frames / 3))" "$seed" > "$out/t$mode.$seed.txt" 2> "$out/t$mode.$seed.err"; then
      echo "FAIL seed $seed: ThreadSanitizer, GXV_SPLIT=$mode"; grep -A12 "WARNING: ThreadSanitizer" "$out/t$mode.$seed.err" | head -30; fail=1
    fi
  done
  echo "seed $seed: $(wc -l < "$out/s0.$seed.txt") frames compared"
done
[ $fail = 0 ] && echo "PASS: GXV_SPLIT=0/1/2 identical, no mismatches, no races (synthetic workload, not Vita performance)"
exit $fail

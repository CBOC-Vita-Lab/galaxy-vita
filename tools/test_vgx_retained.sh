#!/usr/bin/env bash
# GXV_RETAINED (vgx_dlcache.cpp "retained shapes", vgx_state.cpp opcode 0x59) on the host: the J3D-shaped scene of tests/vgx_fe_work.cpp through
# the real front end, every shape's geometry call with its handle in front (FEW_RETAINED=1; =2 with some wrong handles, handle 0 and handles
# without a call), against the same stream pieces without handles (FEW_RETAINED=0).
# Passes when, for each setting around it (GXV_SPLIT=0/1/2, the odd state lists on and off, particles, GXV_DRAWREPLAY=0, GXV_DLCACHE=0,
# GXV_STATICBUF=0, GXV_FE_KNOBS=0, GXV_FECUT_HINT=1, the FE breakdown), the command buffers are the same frame for frame with GXV_RETAINED=0 / 1 / 2;
# GXV_RETAINED=2 finds no mismatch; the retained hits happen; and an AddressSanitizer / UndefinedBehaviorSanitizer build and a ThreadSanitizer
# build (split, jitter) report nothing.
#   tools/test_vgx_retained.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_retained)
# Instruction / cache counts (callgrind, A9-like caches, the game's turn trashing the caches between pictures):
#   FEW_PROFILE=1 FEW_ODD=0 FEW_TRASH=4096 GXV_SPLIT=1 FEW_RETAINED=0|1 GXV_RETAINED=0|1 valgrind --tool=callgrind --cache-sim=yes \
#     --I1=32768,4,32 --D1=32768,4,32 --LL=1048576,8,32 $OUT/few 40 1
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-$MP_TMP/vgx_retained}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/few"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/few-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/few-tsan"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
run() { # run <binary> <env...> -> stdout to $out/run.txt, stderr to $out/run.err
  local bin=$1; shift
  env "$@" "$bin" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
}
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "FEW_ODD=0" "FEW_ODD=0 GXV_SPLIT=1" "FEW_JPA=120" "GXV_DRAWREPLAY=0" "GXV_DRAWREPLAY=2" \
                "GXV_DLCACHE=0" "GXV_STATICBUF=0" "GXV_FE_KNOBS=0" "GXV_FECUT_HINT=1" "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    env $around FEW_RETAINED=0 GXV_RETAINED=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
    for cfg in "FEW_RETAINED=1 GXV_RETAINED=0" "FEW_RETAINED=1 GXV_RETAINED=1" "FEW_RETAINED=1 GXV_RETAINED=2" "FEW_RETAINED=2 GXV_RETAINED=1" "FEW_RETAINED=2 GXV_RETAINED=2"; do
      run "$out/few" $around $cfg
      if ! cmp -s "$out/base.txt" "$out/run.txt"; then
        echo "FAIL seed $seed: $around $cfg differs from no handles"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1
      fi
      if grep -q "retained shapes.*mismatching [1-9]" "$out/run.err"; then echo "FAIL seed $seed: $around $cfg: $(grep 'retained shapes' "$out/run.err")"; fail=1; fi
    done
  done
  # (the hits must happen: on the plain scene most shape calls are hits, and the verify mode checks them)
  FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/count.err"
  set -- $(sed -n 's/.*retained shapes (GXV_RETAINED): \([0-9]*\) calls, \([0-9]*\) hits.*/\1 \2/p' "$out/count.err")
  if [ "${1:-0}" -lt 1000 ] || [ $(( ${2:-0} * 10 )) -lt $(( ${1:-1} * 8 )) ]; then echo "FAIL seed $seed: retained calls ${1:-?}, hits ${2:-?} (want >= 80 %)"; fail=1; fi
  FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=2 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/count.err"
  set -- $(sed -n 's/.*checked \([0-9]*\), mismatching \([0-9]*\)$/\1 \2/p' "$out/count.err" | tail -1)
  if [ "${1:-0}" -lt 1000 ] || [ "${2:-1}" != 0 ]; then echo "FAIL seed $seed: verify checked ${1:-?}, mismatching ${2:-?}"; fail=1; fi
  # sanitizers
  env GXV_SPLIT=1 FEW_RETAINED=0 "$out/few-asan" 30 "$seed" > "$out/asan.base.txt" 2> /dev/null
  for cfg in "FEW_RETAINED=1 GXV_RETAINED=1" "FEW_RETAINED=2 GXV_RETAINED=2"; do
    if ! env GXV_SPLIT=1 $cfg "$out/few-asan" 30 "$seed" > "$out/asan.txt" 2> "$out/asan.err"; then echo "FAIL seed $seed: ASan/UBSan $cfg"; tail -20 "$out/asan.err"; fail=1; fi
    if ! cmp -s "$out/asan.base.txt" "$out/asan.txt"; then echo "FAIL seed $seed: ASan build $cfg differs"; fail=1; fi
  done
  if ! env GXV_SPLIT=1 GXV_SPLIT_JITTER=1 FEW_RETAINED=1 GXV_RETAINED=1 "$out/few-tsan" 30 "$seed" > /dev/null 2> "$out/tsan.err" || grep -q "WARNING: ThreadSanitizer" "$out/tsan.err"; then
    echo "FAIL seed $seed: ThreadSanitizer"; grep -m3 -A8 "WARNING: ThreadSanitizer" "$out/tsan.err"; fail=1
  fi
done
if [ "$fail" = 0 ]; then echo "PASS: test_vgx_retained ($frames frames, seeds $seeds)"; else echo "FAILED"; exit 1; fi

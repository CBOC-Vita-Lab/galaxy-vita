#!/usr/bin/env bash
# GXV_DIRECT_J3D (vgx_direct.h, vgx_direct_draw.inc, vgx_direct_dl.inc) on the host: the J3D-shaped scene of tests/vgx_fe_work.cpp through the
# real front end, every shape's geometry call with its handle in front (FEW_RETAINED=1; =2 with some wrong handles), the retained records on.
# Passes when, for each setting around it (GXV_SPLIT=0/1/2, the odd state lists on and off, particles, the draw replay off, the PS Vita's
# front-end defaults (key ids, compact deltas, hints, TEV input fix, half-precision shaders, edge smoothing with the character notes, rigid
# merging, static culling), shadow-fill bounds (the notes that read the keys), learned copies (declined), the palette rows, GXV_RSCACHE=0
# (declined), the FE breakdown), the command buffers are the same frame for frame with GXV_DIRECT_J3D=0 / 1 / 2 (and with GXV_RETAINED=0,
# which GXV_DIRECT_J3D=1 turns on); GXV_DIRECT_J3D=2 finds no mismatch; the templated runs happen; and an AddressSanitizer /
# UndefinedBehaviorSanitizer build and a ThreadSanitizer build (split, jitter) report nothing.
#   tools/test_vgx_direct_j3d.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_direct_j3d)
# Instruction / cache counts (callgrind, A9-like caches, the game's turn trashing the caches between pictures), the PS Vita's settings:
#   V="GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_SHADOWFILL_BOUNDS=0 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1
#      GXV_RIGIDMERGE=1 GXV_STATICCULL=1"
#   env $V FEW_PROFILE=1 FEW_ODD=0 FEW_TRASH=4096 GXV_SPLIT=1 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=0|1 valgrind --tool=callgrind \
#     --cache-sim=yes --I1=32768,4,32 --D1=32768,4,32 --LL=1048576,8,32 $OUT/few 160 1
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-$MP_TMP/vgx_direct_j3d}
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
vita="GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_SHADOWFILL_BOUNDS=0 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_RIGIDMERGE=1 GXV_STATICCULL=1"
fail=0
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "FEW_ODD=0" "FEW_ODD=0 GXV_SPLIT=1" "FEW_JPA=120" "GXV_DRAWREPLAY=0" \
                "$vita GXV_SPLIT=1" "$vita FEW_ODD=0 GXV_SPLIT=1" "$vita GXV_SPLIT=0" "$vita FEW_JPA=150 FEW_FX=60" "GXV_SHADOWFILL_BOUNDS=1" \
                "GXV_EDGE_AA=1 GXV_CHARAA=1" "GXV_RIGIDMERGE=1 GXV_RIGIDMERGE_DLLEARN=1" "GXV_PALROWS=1" "GXV_RSCACHE=0" "GXV_FE_KNOBS=0" \
                "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    env $around FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
    for cfg in "FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1" "FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=2" "FEW_RETAINED=1 GXV_RETAINED=0 GXV_DIRECT_J3D=1" \
               "FEW_RETAINED=2 GXV_RETAINED=1 GXV_DIRECT_J3D=1" "FEW_RETAINED=2 GXV_RETAINED=1 GXV_DIRECT_J3D=2"; do
      env $around $cfg "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
      if ! cmp -s "$out/base.txt" "$out/run.txt"; then
        echo "FAIL seed $seed: $around $cfg differs from GXV_DIRECT_J3D=0"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1
      fi
      if  grep -q "direct J3D.*verify: [0-9]* checked, [1-9][0-9]* mismatching" "$out/run.err"; then echo "FAIL seed $seed: $around $cfg: $(grep 'direct J3D' "$out/run.err")"; fail=1; fi
    done
  done
  # (the templated runs must happen on the plain scene, with the PS Vita's settings, and the verify mode checks them)
  env $vita FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/count.err"
  set -- $(sed -n 's/.*direct J3D (GXV_DIRECT_J3D): \([0-9]*\) calls armed, \([0-9]*\) runs templated, \([0-9]*\) runs resolved the normal way.*/\1 \2 \3/p' "$out/count.err")
  if [ "${1:-0}" -lt 1000 ] || [ $(( ${2:-0} * 10 )) -lt $(( (${2:-0} + ${3:-1}) * 9 )) ]; then
    echo "FAIL seed $seed: direct calls ${1:-?}, runs templated ${2:-?}, normal ${3:-?} (want >= 1000 calls, >= 90 % templated)"; fail=1
  fi
  env $vita FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=2 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/count.err"
  set -- $(sed -n 's/.*direct J3D.*verify: \([0-9]*\) checked, \([0-9]*\) mismatching$/\1 \2/p' "$out/count.err")
  if [ "${1:-0}" -lt 1000 ] || [ "${2:-1}" != 0 ]; then echo "FAIL seed $seed: verify checked ${1:-?}, mismatching ${2:-?}"; fail=1; fi
  # sanitizers
  env GXV_SPLIT=1 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=0 "$out/few-asan" 30 "$seed" > "$out/asan.base.txt" 2> /dev/null
  for cfg in "FEW_RETAINED=1 GXV_DIRECT_J3D=1" "FEW_RETAINED=2 GXV_DIRECT_J3D=2" "$vita FEW_RETAINED=1 GXV_DIRECT_J3D=1"; do
    if ! env GXV_SPLIT=1 GXV_RETAINED=1 $cfg "$out/few-asan" 30 "$seed" > "$out/asan.txt" 2> "$out/asan.err"; then echo "FAIL seed $seed: ASan/UBSan $cfg"; tail -20 "$out/asan.err"; fail=1; fi
    case "$cfg" in *GXV_FECUT*) ;; *) if ! cmp -s "$out/asan.base.txt" "$out/asan.txt"; then echo "FAIL seed $seed: ASan build $cfg differs"; fail=1; fi ;; esac
  done
  if ! env GXV_SPLIT=1 GXV_SPLIT_JITTER=1 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1 "$out/few-tsan" 30 "$seed" > /dev/null 2> "$out/tsan.err" || grep -q "WARNING: ThreadSanitizer" "$out/tsan.err"; then
    echo "FAIL seed $seed: ThreadSanitizer"; grep -m3 -A8 "WARNING: ThreadSanitizer" "$out/tsan.err"; fail=1
  fi
done
if [ "$fail" = 0 ]; then echo "PASS: test_vgx_direct_j3d ($frames frames, seeds $seeds)"; else echo "FAILED"; exit 1; fi

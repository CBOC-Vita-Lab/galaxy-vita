#!/usr/bin/env bash
# GXV_DL_VARIANTS (vgx_dlcache.cpp "many variants") and GXV_FE_CENSUS (vgx_dlcache.cpp / vgx_draw.cpp "census") on the host: the J3D-shaped
# scene of tests/vgx_fe_work.cpp through the real front end, with fur as FurCtrl::drawFur draws it (FEW_FUR=<layers>: one shape list,
# layers + 1 vertex states) and instanced plants (FEW_PLANTS=<n>).
# Passes when:
#   - with the switches at their defaults (GXV_DL_VARIANTS=8, GXV_FE_CENSUS=0) and with GXV_FE_CENSUS=1 the command buffers are frame for
#     frame those of the plain scene, and (REF_ROOT=<a tree without the change>) those of that tree's front end;
#   - with GXV_DL_VARIANTS=16/32 the tag walk gives the same command buffers as the plain walk (GXV_DL_VARTAG=0), for GXV_SPLIT=0/1/2,
#     GXV_DRAWREPLAY=0, GXV_RETAINED=1 with handles, the odd lists on and off;
#   - GXV_DLCACHE=verify with GXV_DL_VARIANTS=16 checks the walks and finds no mismatch;
#   - fur with 10 layers is bypassed with 8 variants and is not with 16 (the bypassed calls fall back to the scene's own);
#   - an AddressSanitizer / UndefinedBehaviorSanitizer build with the census and 32 variants runs clean.
#   tools/test_vgx_dlvariants.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_dlvariants)
# Cost (callgrind, A9-like caches, the game's turn trashing the caches between pictures; fur calls = FEW_FURN x (layers + 1) a picture):
#   FEW_PROFILE=1 FEW_ODD=0 FEW_TRASH=4096 GXV_SPLIT=1 FEW_FUR=10 GXV_DL_VARIANTS=8|16 valgrind --tool=callgrind --separate-threads=yes \
#     --cache-sim=yes --I1=32768,4,32 --D1=32768,4,32 --LL=1048576,8,32 $OUT/few 40 1
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-$MP_TMP/vgx_dlvariants}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/few"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/few-asan"
if [ -n "${REF_ROOT:-}" ]; then
  refsrcs=(tests/vgx_fe_work.cpp)
  for f in "${srcs[@]:1}"; do refsrcs+=("$REF_ROOT/$f"); done
  $cxx "${common[@]}" -I"$REF_ROOT/platform/vgx/src" -O2 "${refsrcs[@]}" -o "$out/few-ref"
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
same() { # same <label> <envA> -- <envB>: the two runs print the same lines
  local label=$1 a=$2 b=$3
  env $a "$out/few" "$frames" "$seed" > "$out/a.txt" 2> /dev/null
  env $b "${bin_b:-$out/few}" "$frames" "$seed" > "$out/b.txt" 2> /dev/null
  if ! cmp -s "$out/a.txt" "$out/b.txt"; then echo "FAIL seed $seed: $label: [$a] differs from [$b]"; diff "$out/a.txt" "$out/b.txt" | head -4; fail=1; fi
}
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "FEW_ODD=0" "FEW_JPA=120" "GXV_DRAWREPLAY=0" "FEW_RETAINED=1 GXV_RETAINED=1" \
                "FEW_FUR=10" "FEW_FUR=10 GXV_SPLIT=1" "FEW_FUR=6 FEW_FURN=3" "FEW_PLANTS=40" "FEW_FUR=12 FEW_PLANTS=40 FEW_ODD=0"; do
    # defaults and the census leave everything as it was
    same "defaults" "$around" "$around GXV_DL_VARIANTS=8 GXV_FE_CENSUS=0"
    same "census" "$around" "$around GXV_FE_CENSUS=1"
    # the tag walk is the plain walk
    for v in 16 32; do
      same "tag walk" "$around GXV_DL_VARIANTS=$v" "$around GXV_DL_VARIANTS=$v GXV_DL_VARTAG=0"
      same "tag walk + census" "$around GXV_DL_VARIANTS=$v" "$around GXV_DL_VARIANTS=$v GXV_FE_CENSUS=1"
    done
    if [ -n "${REF_ROOT:-}" ]; then
      bin_b="$out/few-ref" same "against REF_ROOT" "$around" "$around"
      unset bin_b
    fi
  done
  # verify mode: the walks are checked and agree
  FEW_FUR=10 GXV_DLCACHE=verify GXV_DL_VARIANTS=16 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/verify.err"
  set -- $(sed -n 's/.*variant walks verified \([0-9]*\) (differing \([0-9]*\))$/\1 \2/p' "$out/verify.err")
  if [ "${1:-0}" -lt 1000 ] || [ "${2:-1}" != 0 ] || grep -q "MISMATCH variant walk" "$out/verify.err"; then echo "FAIL seed $seed: verify: checked ${1:-?}, mismatching ${2:-?}"; fail=1; fi
  # the fur is bypassed with 8 variants and cached with 16
  byp() { env "$@" "$out/few" "$frames" "$seed" 2>&1 > /dev/null | sed -n 's/.*bypassed \([0-9]*\), builds.*/\1/p'; }
  b0=$(byp FEW_ODD=0); b8=$(byp FEW_ODD=0 FEW_FUR=10 GXV_DL_VARIANTS=8); b16=$(byp FEW_ODD=0 FEW_FUR=10 GXV_DL_VARIANTS=16)
  if [ "$b8" -lt $(( b0 + 1000 )) ] || [ "$b16" -gt $(( b0 + 100 )) ]; then echo "FAIL seed $seed: bypassed calls: scene $b0, fur with 8 variants $b8, with 16 $b16"; fail=1; fi
  echo "seed $seed: bypassed calls in $frames pictures: scene $b0, + fur (10 layers) with 8 variants $b8, with 16 $b16"
  # sanitizers, census and the largest cap
  if ! env FEW_LOG=1 FEW_FUR=12 FEW_PLANTS=40 GXV_FE_CENSUS=1 GXV_DL_VARIANTS=32 GXV_SPLIT=1 "$out/few-asan" 400 "$seed" > /dev/null 2> "$out/asan.err" || \
     grep -q "ERROR: AddressSanitizer\|runtime error" "$out/asan.err"; then echo "FAIL seed $seed: ASan/UBSan"; tail -20 "$out/asan.err"; fail=1; fi
  if ! grep -q "fe-census: static runs per picture" "$out/asan.err"; then echo "FAIL seed $seed: no census line"; fail=1; fi
done
if [ "$fail" = 0 ]; then echo "PASS: test_vgx_dlvariants ($frames frames, seeds $seeds)"; else echo "FAILED"; exit 1; fi

#!/usr/bin/env bash
# GXV_FE_PARALLEL (vgx_fepar.h: streamed draws' vertex decodes on the front-end split's finishing thread) on the host, through the real front
# end: the J3D-shaped scene of tests/vgx_fe_work.cpp plus FEW_BIG large streamed draws a picture (direct, indexed, PNMTXIDX, 8-bit, small
# ones, lines, display lists the cache bypasses, draw-sync tokens between them).
# Passes when:
#   - for every setting around it, GXV_FE_PARALLEL=1 and =2 give the command buffers of =0 frame for frame (records, vertex / index /
#     uniform arenas, uploads: the fixture's hash), decodes do go to the finishing thread (=1) and =2 checks them with 0 mismatches;
#   - with the split off (GXV_SPLIT=0) or verifying (GXV_SPLIT=2) nothing goes, and the buffers are those of =0 still;
#   - REF_ROOT=<a tree without the change> (default /home/claude/g23/source/source): the fixture built against that front end gives the
#     same buffers as this one with =0 and =1 (the change leaves the single-thread path as it was);
#   - an AddressSanitizer / UndefinedBehaviorSanitizer build and a ThreadSanitizer build report nothing (=1, =2);
#   - the negative controls fail as they must: VGX_FEPAR_NEG=1 (a bit of each off-thread decode flipped) changes the buffers and =2 reports
#     mismatches; VGX_FEPAR_NEG=2 (the finishing thread bumps the counter the player bumps at every Send) is reported by ThreadSanitizer.
#   tools/test_vgx_fepar.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_fepar). Builds one at a time.
set -uo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2 3"}
out=${OUT:-$MP_TMP/vgx_fepar}
ref=${REF_ROOT-/home/claude/g23/source/source}
mkdir -p "$out"
cxx=${CXX:-g++}
fe=(platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp
    platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp
    platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g0 -pipe -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread -w)
build() { # name, extra flags...
  local name=$1; shift
  [ -x "$out/$name" ] && [ -z "${REBUILD:-}" ] && return 0
  echo "building $name"
  $cxx "${common[@]}" "$@" tests/vgx_fe_work.cpp "${fe[@]}" -o "$out/$name" || { echo "FAIL: build $name"; exit 1; }
}
build few -O2
build few-asan -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
build few-tsan -O1 -fsanitize=thread
build few-neg1 -O2 -DVGX_FEPAR_NEG=1
build few-neg2 -O1 -fsanitize=thread -DVGX_FEPAR_NEG=2
if [ -n "$ref" ] && [ -d "$ref/platform/vgx/src" ] && [ ! -x "$out/few-ref" ]; then
  echo "building few-ref against $ref"
  refsrcs=(); for f in "${fe[@]}"; do refsrcs+=("$ref/$f"); done
  $cxx -std=c++17 -g0 -pipe -I"$ref/platform/vgx/src" -I"$ref/platform/vgx/third_party" -pthread -w -O2 tests/vgx_fe_work.cpp "${refsrcs[@]}" -o "$out/few-ref" ||
    { echo "FAIL: build few-ref"; exit 1; }
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=0
fail=0
sent() { sed -n 's/.*GXV_FE_PARALLEL): \([0-9]*\) sent.*/\1/p' "$1"; }
checked() { sed -n 's/.*checked \([0-9]*\), mismatching \([0-9]*\); settles.*/\1 \2/p' "$1"; }
for seed in $seeds; do
  for wl in "FEW_BIG=30" "FEW_BIG=20 FEW_JPA=120 FEW_FX=60" "FEW_BIG=40 FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1"; do
    for around in "GXV_SPLIT=1" "GXV_SPLIT=1 GXV_FXJOIN=1" "GXV_SPLIT=1 GXV_SPLIT_POLL_US=150" "GXV_SPLIT=1 GXV_SPLIT_COPY=1 GXV_RIGIDMERGE_DLMAX=64" \
                  "GXV_SPLIT=1 GXV_FE_PARALLEL_MIN=1" "GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=1 GXV_CHARAA=1" "GXV_SPLIT=1 GXV_SPLIT_PUBLISH=1 GXV_SPLIT_WAKE_JOBS=4" \
                  "GXV_SPLIT=1 GXV_SOFTXFORM=128" "GXV_SPLIT=1 GXV_FE_PARALLEL_PROF=1"; do
      env $wl $around GXV_FE_PARALLEL=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
      for m in 1 2; do
        env $wl $around GXV_FE_PARALLEL=$m "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
        if ! cmp -s "$out/base.txt" "$out/run.txt"; then
          echo "FAIL seed $seed: $wl $around GXV_FE_PARALLEL=$m differs from =0"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1
        fi
        n=$(sent "$out/run.err"); set -- $(checked "$out/run.err")
        if [ "${n:-0}" -lt 50 ]; then echo "FAIL seed $seed: $wl $around GXV_FE_PARALLEL=$m: only ${n:-?} decodes went"; fail=1; fi
        if [ $m = 2 ] && { [ "${1:-0}" -lt 50 ] || [ "${2:-1}" != 0 ]; }; then echo "FAIL seed $seed: $wl $around =2: checked ${1:-?}, mismatching ${2:-?}"; fail=1; fi
        if grep -q "fepar: MISMATCH" "$out/run.err"; then echo "FAIL seed $seed: $wl $around: $(grep -m1 'fepar: MISMATCH' "$out/run.err")"; fail=1; fi
      done
      if [ -x "$out/few-ref" ] && [ "$around" = "GXV_SPLIT=1" ]; then
        env $wl $around "$out/few-ref" "$frames" "$seed" > "$out/ref.txt" 2> /dev/null
        if ! cmp -s "$out/ref.txt" "$out/base.txt"; then echo "FAIL seed $seed: $wl: =0 differs from the front end of $ref"; diff "$out/ref.txt" "$out/base.txt" | head -4; fail=1; fi
      fi
    done
    # (split off / verifying: nothing goes; the buffers are =0's)
    for sp in 0 2; do
      env $wl GXV_SPLIT=$sp GXV_FE_PARALLEL=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
      env $wl GXV_SPLIT=$sp GXV_FE_PARALLEL=1 "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
      if ! cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL seed $seed: $wl GXV_SPLIT=$sp: GXV_FE_PARALLEL=1 differs from =0"; fail=1; fi
      if [ "$(sent "$out/run.err")" != 0 ]; then echo "FAIL seed $seed: $wl GXV_SPLIT=$sp: decodes went ($(sent "$out/run.err"))"; fail=1; fi
    done
  done
  # sanitizers (fewer frames)
  for m in 1 2; do
    for wl in "FEW_BIG=30" "FEW_BIG=20 FEW_JPA=80"; do
      env $wl GXV_SPLIT=1 GXV_FE_PARALLEL=$m "$out/few-asan" 30 "$seed" > /dev/null 2> "$out/asan.err" || { echo "FAIL seed $seed: ASan/UBSan $wl =$m: exit $?"; tail -5 "$out/asan.err"; fail=1; }
      if grep -q "ERROR: AddressSanitizer\|runtime error" "$out/asan.err"; then echo "FAIL seed $seed: ASan/UBSan $wl =$m"; grep -m3 "ERROR\|runtime error" "$out/asan.err"; fail=1; fi
      env $wl GXV_SPLIT=1 GXV_FE_PARALLEL=$m "$out/few-tsan" 30 "$seed" > /dev/null 2> "$out/tsan.err"
      if grep -q "WARNING: ThreadSanitizer" "$out/tsan.err"; then echo "FAIL seed $seed: ThreadSanitizer $wl =$m"; grep -m1 -A12 "WARNING: ThreadSanitizer" "$out/tsan.err"; fail=1; fi
    done
  done
done
# negative controls
seed=${seeds%% *}
FEW_BIG=30 GXV_SPLIT=1 GXV_FE_PARALLEL=0 "$out/few-neg1" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
FEW_BIG=30 GXV_SPLIT=1 GXV_FE_PARALLEL=1 "$out/few-neg1" "$frames" "$seed" > "$out/run.txt" 2> /dev/null
if cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL: negative control VGX_FEPAR_NEG=1: =1 gave the same buffers"; fail=1; else echo "negative control 1 (=1): buffers differ, as they must"; fi
FEW_BIG=30 GXV_SPLIT=1 GXV_FE_PARALLEL=2 "$out/few-neg1" "$frames" "$seed" > /dev/null 2> "$out/run.err"
set -- $(checked "$out/run.err")
if [ "${2:-0}" = 0 ]; then echo "FAIL: negative control VGX_FEPAR_NEG=1: =2 found no mismatch"; fail=1; else echo "negative control 1 (=2): ${2} of ${1} checks mismatching, as they must"; fi
FEW_BIG=30 GXV_SPLIT=1 GXV_FE_PARALLEL=1 "$out/few-neg2" 30 "$seed" > /dev/null 2> "$out/tsan.err"
if grep -q "WARNING: ThreadSanitizer" "$out/tsan.err"; then echo "negative control 2: ThreadSanitizer reports the race, as it must"; else echo "FAIL: negative control VGX_FEPAR_NEG=2: no race reported"; fail=1; fi
if [ $fail = 0 ]; then echo "PASS"; else echo "FAIL"; fi
exit $fail

#!/usr/bin/env bash
# GXV_DRAWREPLAY on the host: the synthetic game-shaped scene of tests/vgx_drawreplay.cpp (the same objects in the same order every frame, with
# objects coming and going, lists moving and rewritten, dynamic geometry, a list's memory reused) through the real front end.
# Passes when GXV_DRAWREPLAY=0, 1 and 2 give the same command buffers frame for frame and learn the same shader warm-up list, with the
# front-end split off and on, with the look-ahead on (GXV_FE_PREFETCH=4) and in the old state-list mode (GXV_SLFAST=0); when replays
# actually happen (most geometry calls); and when an AddressSanitizer / UndefinedBehaviorSanitizer build and a ThreadSanitizer build
# (split, jitter) report nothing.
# REF_ROOT=<a source tree without the change> (optional): the same fixture is also built against that tree's front end, and its command
# buffers must be the ones this tree makes, with the switches on and off (the parts of the change that are not switched: the cache
# entry's layout, the sweep schedule's place).
#   tools/test_vgx_drawreplay.sh [frames] [seeds]      (CXX: host compiler, default g++; out-of-tree build in $OUT, default /tmp/vgx_drawreplay)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-400}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_drawreplay}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_drawreplay.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/drp"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/drp-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/drp-tsan"
if [ -n "${REF_ROOT:-}" ]; then
  refsrcs=(tests/vgx_drawreplay.cpp)
  for f in "${srcs[@]:1}"; do refsrcs+=("$REF_ROOT/$f"); done
  $cxx "${common[@]}" -I"$REF_ROOT/platform/vgx/src" -O2 "${refsrcs[@]}" -o "$out/drp-ref"
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# (the Vita's limit for the CPU-transformed draws: most geometry lists become static buffers, as there)
export GXV_SOFTXFORM=32
fail=0
for seed in $seeds; do
  rm -f "$out"/list.*."$seed"
  DRP_LIST="$out/list.ref.$seed" GXV_DRAWREPLAY=0 GXV_SPLIT=0 "$out/drp" "$frames" "$seed" > "$out/ref.$seed.txt" 2> "$out/ref.$seed.err"
  for cfg in "1 0 0 1" "2 0 0 1" "1 1 0 1" "2 1 0 1" "0 1 0 1" "1 0 4 1" "1 1 4 1" "1 0 0 0" "0 0 0 0"; do
    set -- $cfg
    tag="r$1s$2p$3f$4"
    DRP_LIST="$out/list.$tag.$seed" GXV_DRAWREPLAY=$1 GXV_SPLIT=$2 GXV_FE_PREFETCH=$3 GXV_SLFAST=$4 "$out/drp" "$frames" "$seed" > "$out/$tag.$seed.txt" 2> "$out/$tag.$seed.err"
    if ! cmp -s "$out/ref.$seed.txt" "$out/$tag.$seed.txt"; then echo "FAIL seed $seed: GXV_DRAWREPLAY=$1 GXV_SPLIT=$2 GXV_FE_PREFETCH=$3 GXV_SLFAST=$4 differs"; diff "$out/ref.$seed.txt" "$out/$tag.$seed.txt" | head -4; fail=1; fi
    if ! cmp -s "$out/list.ref.$seed" "$out/list.$tag.$seed"; then echo "FAIL seed $seed: $tag: shader warm-up lists differ"; fail=1; fi
  done
  if [ -n "${REF_ROOT:-}" ]; then
    GXV_SPLIT=0 "$out/drp-ref" "$frames" "$seed" > "$out/pristine.$seed.txt" 2> /dev/null
    if ! cmp -s "$out/ref.$seed.txt" "$out/pristine.$seed.txt"; then echo "FAIL seed $seed: differs from the front end of $REF_ROOT"; diff "$out/ref.$seed.txt" "$out/pristine.$seed.txt" | head -4; fail=1; fi
  fi
  # (replays must actually happen: at least half the geometry calls)
  hits=$(sed -n 's/.*replay hits \([0-9]*\), misses \([0-9]*\).*recorded \([0-9]*\).*/\1 \2 \3/p' "$out/r1s0p0f1.$seed.err")
  set -- $hits
  if [ "${1:-0}" -lt "${3:-1}" ]; then echo "FAIL seed $seed: too few replays ($hits: hits misses recorded)"; fail=1; fi
  for cfg in "1 0" "2 1"; do
    set -- $cfg
    if ! GXV_DRAWREPLAY=$1 GXV_SPLIT=$2 "$out/drp-asan" "$((frames / 2))" "$seed" > "$out/asan.$seed.txt" 2> "$out/asan.$seed.err"; then
      echo "FAIL seed $seed: sanitizers, GXV_DRAWREPLAY=$1 GXV_SPLIT=$2"; head -30 "$out/asan.$seed.err"; fail=1
    fi
    if ! GXV_DRAWREPLAY=0 GXV_SPLIT=$2 "$out/drp-asan" "$((frames / 2))" "$seed" > "$out/asan0.$seed.txt" 2> /dev/null || ! cmp -s "$out/asan.$seed.txt" "$out/asan0.$seed.txt"; then
      echo "FAIL seed $seed: sanitizer build, GXV_DRAWREPLAY=$1 against 0 differs (GXV_SPLIT=$2)"; fail=1
    fi
  done
  if ! TSAN_OPTIONS="halt_on_error=1" GXV_SPLIT_JITTER=1 GXV_DRAWREPLAY=1 GXV_SPLIT=1 "$out/drp-tsan" "$((frames / 4))" "$seed" > "$out/tsan.$seed.txt" 2> "$out/tsan.$seed.err"; then
    echo "FAIL seed $seed: ThreadSanitizer, GXV_SPLIT=1"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsan.$seed.err" | head -30; fail=1
  fi
  echo "seed $seed: $(wc -l < "$out/ref.$seed.txt") frames compared; $(tail -1 "$out/r1s0p0f1.$seed.err")"
done
[ $fail = 0 ] && echo "PASS: GXV_DRAWREPLAY=0/1/2 identical (GXV_SPLIT=0/1, GXV_FE_PREFETCH=0/4, GXV_SLFAST=0/1); sanitizers and ThreadSanitizer clean (synthetic scene, not Vita timing)"
exit $fail

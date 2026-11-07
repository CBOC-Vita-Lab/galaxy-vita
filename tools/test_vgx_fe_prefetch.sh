#!/usr/bin/env bash
# GXV_FE_PREFETCH / GXV_VARFRONT on the host: the synthetic game-shaped stream of tests/vgx_fe_prefetch.cpp through the real front end.
# Passes when every look-ahead depth (0..4), GXV_VARFRONT=0/1 and GXV_SPLIT=0/1 give the same command buffers frame for frame and learn
# the same shader warm-up list, an AddressSanitizer / UndefinedBehaviorSanitizer build reports nothing (the look-ahead reads only
# the stream it is given), and a ThreadSanitizer build with the front-end split and its jitter reports no race.
#   tools/test_vgx_fe_prefetch.sh [frames] [seeds]      (CXX: host compiler, default g++; out-of-tree build in $OUT, default /tmp/vgx_fe_prefetch)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-120}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_fe_prefetch}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_prefetch.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/fepf"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/fepf-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/fepf-tsan"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
for seed in $seeds; do
  rm -f "$out"/list.*."$seed"
  FEPF_LIST="$out/list.ref.$seed" GXV_FE_PREFETCH=0 GXV_VARFRONT=0 GXV_SPLIT=0 "$out/fepf" "$frames" "$seed" > "$out/ref.$seed.txt" 2> "$out/ref.$seed.err"
  for cfg in "0 1 0" "1 1 0" "2 1 0" "3 1 0" "4 1 0" "4 0 0" "4 1 1" "0 1 1"; do
    set -- $cfg
    tag="p$1v$2s$3"
    FEPF_LIST="$out/list.$tag.$seed" GXV_FE_PREFETCH=$1 GXV_VARFRONT=$2 GXV_SPLIT=$3 "$out/fepf" "$frames" "$seed" > "$out/$tag.$seed.txt" 2> "$out/$tag.$seed.err"
    if ! cmp -s "$out/ref.$seed.txt" "$out/$tag.$seed.txt"; then echo "FAIL seed $seed: GXV_FE_PREFETCH=$1 GXV_VARFRONT=$2 GXV_SPLIT=$3 differs"; diff "$out/ref.$seed.txt" "$out/$tag.$seed.txt" | head -4; fail=1; fi
    if ! cmp -s "$out/list.ref.$seed" "$out/list.$tag.$seed"; then echo "FAIL seed $seed: $tag: shader warm-up lists differ"; fail=1; fi
  done
  for cfg in "4 1 0" "4 1 1"; do
    set -- $cfg
    if ! GXV_FE_PREFETCH=$1 GXV_VARFRONT=$2 GXV_SPLIT=$3 "$out/fepf-asan" "$((frames / 2))" "$seed" > "$out/asan.$seed.txt" 2> "$out/asan.$seed.err"; then
      echo "FAIL seed $seed: sanitizers, GXV_FE_PREFETCH=$1 GXV_SPLIT=$3"; head -30 "$out/asan.$seed.err"; fail=1
    fi
  done
  if ! TSAN_OPTIONS="halt_on_error=1" GXV_SPLIT_JITTER=1 GXV_FE_PREFETCH=4 GXV_SPLIT=1 "$out/fepf-tsan" "$((frames / 3))" "$seed" > "$out/tsan.$seed.txt" 2> "$out/tsan.$seed.err"; then
    echo "FAIL seed $seed: ThreadSanitizer, GXV_SPLIT=1"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsan.$seed.err" | head -30; fail=1
  fi
  echo "seed $seed: $(wc -l < "$out/ref.$seed.txt") frames compared; $(tail -1 "$out/ref.$seed.err")"
done
[ $fail = 0 ] && echo "PASS: GXV_FE_PREFETCH=0..4, GXV_VARFRONT=0/1, GXV_SPLIT=0/1 identical; sanitizers and ThreadSanitizer clean (synthetic stream, not Vita timing)"
exit $fail

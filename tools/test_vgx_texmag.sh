#!/usr/bin/env bash
# GXV_TEXMAG_CENSUS (platform/vgx/src/vgx_texmag.inc) on the host.
#  1. tests/vgx_texmag.cpp: known geometry (distance, texture-matrix scale, a clipped quad, GPU-transform path, minified quads, a tilted floor
#     against a brute-force pixel count) -- plain, ASan+UBSan and TSan builds, GXV_SOFTXFORM=32 and 0, GXV_SPLIT=0/1.
#  2. tests/vgx_fe_work.cpp (J3D-shaped lists through the real front end): the census on (every picture, GXV_TEXMAG_CENSUS=1, and 3) must give
#     the same command buffers frame for frame as off, over the switches below, and the ASan/UBSan and TSan builds must report nothing.
#   tools/test_vgx_texmag.sh [frames] [seeds]        (build in $OUT, default /tmp/vgx_texmag)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-60}
seeds=${2:-"1 2"}
out=${OUT:-/tmp/vgx_texmag}
mkdir -p "$out"
cxx=${CXX:-g++}
lib=(platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
     platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
     platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
san=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
$cxx "${common[@]}" -O2 tests/vgx_texmag.cpp "${lib[@]}" -o "$out/tm"
$cxx "${common[@]}" "${san[@]}" tests/vgx_texmag.cpp "${lib[@]}" -o "$out/tm-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread tests/vgx_texmag.cpp "${lib[@]}" -o "$out/tm-tsan"
$cxx "${common[@]}" -O2 tests/vgx_fe_work.cpp "${lib[@]}" -o "$out/few"
$cxx "${common[@]}" "${san[@]}" tests/vgx_fe_work.cpp "${lib[@]}" -o "$out/few-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread tests/vgx_fe_work.cpp "${lib[@]}" -o "$out/few-tsan"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1
fail=0
wd="$out/run"; rm -rf "$wd"; mkdir -p "$wd"; cd "$wd"
for bin in tm tm-asan tm-tsan; do
  for env in "GXV_SOFTXFORM=32 GXV_SPLIT=0" "GXV_SOFTXFORM=0 GXV_SPLIT=1"; do
    if ! env $env "$out/$bin" > "$out/$bin.log" 2>&1; then echo "FAIL $bin $env"; grep -E "FAIL|ERROR|WARNING" "$out/$bin.log" | head -5; fail=1; else echo "ok   $bin $env: $(tail -1 "$out/$bin.log")"; fi
  done
done
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "FEW_RETAINED=1 GXV_SPLIT=1" "FEW_J3N=1 GXV_SPLIT=1" "FEW_JPA=64 GXV_SPLIT=1" "FEW_JPA=64 FEW_JPAVAR=1 GXV_SPLIT=0" "FEW_FUR=3 GXV_SPLIT=1" "FEW_S16=1 GXV_SPLIT=1"; do
    env $around GXV_TEXMAG_CENSUS=0 "$out/few" "$frames" "$seed" > "$out/base.txt" 2> "$out/base.err"
    for every in 1 3; do
      # (GXV_TEXMAG_ANYWRITE=1 MINSIDE=4: the fixture's draws never set the colour-write bits, and its textures are small: without the two knobs the census would skip them all)
      env $around GXV_TEXMAG_CENSUS=$every GXV_TEXMAG_ANYWRITE=1 GXV_TEXMAG_MINSIDE=4 GXV_TEXMAG_DIR="$wd/" GXV_TEXMAG_WRITE_S=5 "$out/few" "$frames" "$seed" > "$out/run.txt" 2> "$out/run.err"
      if ! cmp -s "$out/base.txt" "$out/run.txt"; then echo "FAIL seed $seed: $around census=$every: command buffers differ"; diff "$out/base.txt" "$out/run.txt" | head -4; fail=1; fi
    done
  done
  for bin in few-asan few-tsan; do
    env GXV_SPLIT=1 GXV_TEXMAG_CENSUS=1 GXV_TEXMAG_ANYWRITE=1 GXV_TEXMAG_MINSIDE=4 GXV_TEXMAG_DIR="$wd/" "$out/$bin" 30 "$seed" > /dev/null 2> "$out/$bin.err" || { echo "FAIL $bin seed $seed"; tail -20 "$out/$bin.err"; fail=1; }
    if grep -qE "ERROR: AddressSanitizer|runtime error|WARNING: ThreadSanitizer" "$out/$bin.err"; then echo "FAIL $bin seed $seed: sanitizer report"; fail=1; fi
  done
done
m=$(grep -h "^measured_draws=" "$wd"/texmag-00000000.txt 2>/dev/null | head -1)
echo "census on the fe_work fixture (last run): ${m:-no file}"
case "$m" in measured_draws=0|"") echo "FAIL: the census measured nothing on the fixture, so the identity proof is empty"; fail=1;; esac
[ $fail = 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }

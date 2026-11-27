#!/usr/bin/env bash
# GXV_COLSHADOW_CLIP (dreadblend25): the pure clip (tests/colshadow_clip_test.cpp: area conservation by dense sampling, wholly-inside unchanged, watertight cuts,
# negative controls) and the real front end + GLES back end on Mesa (tests/vgx_colshadow_clip_gl.cpp: draw3's two passes with whole triangles vs clipped fans, picture
# hash and PSNR, negative controls). No game data.   tools/test_vgx_colshadow_clip_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$root/platform/vgx/src/$unit.cpp"); done
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/platform/include" "$root/tests/colshadow_clip_test.cpp" -o "$out/clip"
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
  "$root/tests/vgx_colshadow_clip_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/gl"
"$out/clip" | tee "$out/clip.log" | tail -4
grep -q FAIL "$out/clip.log" && { echo "FAILED: pure clip test"; exit 1; }
env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0 "$out/gl" 2>&1 | grep -v '^libunwind' | tee "$out/gl.log"
grep -q FAIL "$out/gl.log" && { echo "FAILED: GL test"; exit 1; }
grep -q '^frame5' "$out/gl.log" || { echo "FAILED: no picture"; exit 1; }
echo "colshadow clip: all checks passed"

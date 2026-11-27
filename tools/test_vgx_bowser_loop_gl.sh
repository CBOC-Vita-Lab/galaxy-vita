#!/usr/bin/env bash
# bowser24: the back end's work per picture across many scene changes stays flat (tests/vgx_bowser_loop_gl.cpp): the production GX front end and
# GLES back end on Mesa, no game data, no Vita. Khronos headers: KHR_INCLUDE (default /work/khr/include).
#   tools/test_vgx_bowser_loop_gl.sh [output-dir]       (BOWSER_SCENES=60 BOWSER_PICS=12 BOWSER_TOL=2.0)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_hdtex vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${BOWSER_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_bowser_loop_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_bowser_loop_gl"
fi
env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_SHADER_RECLAIM=1 \
  "$out/vgx_bowser_loop_gl" 2>&1 | grep -v "^libunwind"
echo "(build and run in $out)"

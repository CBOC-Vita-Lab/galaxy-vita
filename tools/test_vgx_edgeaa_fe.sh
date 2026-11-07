#!/usr/bin/env bash
# GXV_EDGE_AA, the front end's side (tests/vgx_edgeaa_fe_test.cpp): the production GX front end and GLES back end on Mesa; the HUD
# rectangles each display copy hands to the back end, with the switch on and off.
#   tools/test_vgx_edgeaa_fe.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
  "$root/tests/vgx_edgeaa_fe_test.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_edgeaa_fe"
common=(LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0)
env "${common[@]}" GXV_EDGE_AA=1 "$out/vgx_edgeaa_fe"
env -u GXV_EDGE_AA "${common[@]}" "$out/vgx_edgeaa_fe"

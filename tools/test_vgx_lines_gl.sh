#!/usr/bin/env bash
# GXV_FIX_LINES: GX lines and points at their GX size, on the production pipeline and Mesa (pixel comparison with a reference rasteriser,
# texture coordinate offsets, strips, an end point behind the eye, palette matrices); no game data. Runs with the switch unset (default
# off), =0 and =1, with the front-end split off and on.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
units=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
       vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
nice "${CXX:-clang++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_lines_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
  -lEGL -lGLESv2 -pthread -o "$out/vgx_lines_gl"
run() { # <expect> [env assignment]
  local expect=$1; shift
  for split in 0 1; do
    env -u GXV_FIX_LINES "$@" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_SPLIT="$split" GXV_FXHALF=0 \
      GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 nice "$out/vgx_lines_gl" "$expect" 2>&1 | grep -v '^libunwind' | grep -vE '^\[(autoq|vgx)\]|^fesplit'
  done
}
echo "== GXV_FIX_LINES unset (default off)"; run 0 env
echo "== GXV_FIX_LINES=0"; run 0 env GXV_FIX_LINES=0
echo "== GXV_FIX_LINES=1"; run 1 env GXV_FIX_LINES=1
echo "== GXV_FIX_LINES=1, render target 2x and 3x the EFB (widths scale with it)"; run 1 env GXV_FIX_LINES=1 LINES_SCALE=2; run 1 env GXV_FIX_LINES=1 LINES_SCALE=3

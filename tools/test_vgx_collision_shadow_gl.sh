#!/usr/bin/env bash
# Renderer source and evidence paths must be explicit; no game data required.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
renderer=${1:?renderer source worktree}
out=${2:?private output directory}
mkdir -p "$out"
src="$renderer/platform/vgx/src"
units=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
       vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-clang++}" -std=c++20 -O1 -g -I"$src" -I"$renderer/platform/vgx/third_party" \
  "$root/tests/vgx_collision_shadow_gl.cpp" "${args[@]}" \
  "$renderer/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread \
  -o "$out/vgx-collision-shadow"
for split in 0 1; do
  for zero in 0 1; do
    env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 \
      GXV_SPLIT="$split" GXV_DSTALPHA_ZERO="$zero" GXV_FXHALF=0 GXV_PASSHALF=0 \
      GXV_CROWD=0 GXV_EFBSWAP=0 "$out/vgx-collision-shadow" "$zero" \
      >"$out/split-$split-zero-$zero.log" 2>&1
  done
done

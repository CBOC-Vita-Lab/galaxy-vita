#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
         vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_PIPELINE_SKIP_BUILD:-0}" != 1 ]]; then
  "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_clipfield_pipeline_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_clipfield_pipeline_gl"
fi
for split in 0 1; do
  for zero in 0 1; do
    for size in '128 96' '96 72' '64 48'; do
      read -r w h <<< "$size"
      env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 \
        GXV_SPLIT="$split" GXV_DSTALPHA_ZERO="$zero" GXV_FXHALF=0 GXV_PASSHALF=0 \
        GXV_CROWD=0 GXV_EFBSWAP=0 "$out/vgx_clipfield_pipeline_gl" "$w" "$h" \
        2>&1 | tee "$out/split${split}-zero${zero}-${w}x${h}.log"
    done
  done
done
for optimized in 0 1; do
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 \
    GXV_SPLIT=1 GXV_DSTALPHA_ZERO=1 GXV_FXHALF=0 GXV_CROWD=0 \
    GXV_PASSISO="$optimized" GXV_PASSDIRECT2="$optimized" GXV_PASSHOIST="$optimized" \
    GXV_DEPTHPLAN="$optimized" GXV_PASSHALF="$optimized" GXV_EFBSWAP="$optimized" \
    GXV_PASSCHECK=1 GXV_PASSLOG=1 "$out/vgx_clipfield_pipeline_gl" 64 48 \
    2>&1 | tee "$out/optimized${optimized}-64x48.log"
done

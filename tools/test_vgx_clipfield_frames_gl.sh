#!/usr/bin/env bash
# Several frames of the clip-field chain in a row (tests/vgx_clipfield_frames_gl.cpp): every combination of the
# frame-to-frame optimisations the console runs with (pass isolation, constant-copy replacement, no-op clears,
# EFB hand-over, copy atlas) against the same chain with all of them off.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
         vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_FRAMES_SKIP_BUILD:-0}" != 1 ]]; then
  "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_clipfield_frames_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_clipfield_frames_gl"
fi
status=0
for optimized in 0 1; do
  for split in 0 1; do
    for size in '128 96' '64 48'; do
      read -r w h <<< "$size"
      env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_SPLIT="$split" GXV_DSTALPHA_ZERO=1 \
        GXV_FXHALF=0 GXV_CROWD=0 GXV_PASSHALF=0 \
        GXV_PASSISO="$optimized" GXV_PASSDIRECT2="$optimized" GXV_PASSHOIST="$optimized" GXV_DEPTHPLAN="$optimized" \
        GXV_EFBSWAP="$optimized" GXV_DEADCOPY="$optimized" GXV_NOOPCLEAR="$optimized" GXV_ATLAS=$((optimized * 2)) \
        "$out/vgx_clipfield_frames_gl" "$w" "$h" 2>&1 | tee "$out/opt${optimized}-split${split}-${w}x${h}.log" || status=1
    done
  done
done
exit $status

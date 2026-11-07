#!/usr/bin/env bash
# Standalone real-GLES renderer probe; no game data or Vita required.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
         vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_clipfield_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
  -lEGL -lGLESv2 -pthread -o "$out/vgx_clipfield_gl"
for value in unset 0 invalid 1; do
  mode=legacy
  [[ "$value" == 1 ]] && mode=zero
  flag=(-u GXV_DSTALPHA_ZERO)
  [[ "$value" != unset ]] && flag=("GXV_DSTALPHA_ZERO=$value")
  env "${flag[@]}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 \
    GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$out/vgx_clipfield_gl" "$mode" 2>&1 | tee "$out/$value.log"
done

#!/usr/bin/env bash
# GX texture LOD bias (GXV_FIX_LODBIAS, GAL-013 / GAL-019) on the production pipeline and Mesa; no game data.
# Runs with the switch unset (default off: the bias is ignored), =1, and =0; plus the shader text of an unflagged key.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
units=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
       vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-clang++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_lodbias_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
  -lEGL -lGLESv2 -pthread -o "$out/vgx_lodbias_gl"
run() { # <expect> [env assignment]
  local expect=$1; shift
  for split in 0 1; do
    env -u GXV_FIX_LODBIAS "$@" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_SPLIT="$split" GXV_FXHALF=0 \
      GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 "$out/vgx_lodbias_gl" "$expect" 2>&1 | grep -v '^libunwind' | grep -vE '^\[(autoq|vgx)\]|^fesplit'
  done
}
echo "== GXV_FIX_LODBIAS unset (default off)"; run 0 env
echo "== GXV_FIX_LODBIAS=1"; run 1 env GXV_FIX_LODBIAS=1
echo "== GXV_FIX_LODBIAS=0"; run 0 env GXV_FIX_LODBIAS=0

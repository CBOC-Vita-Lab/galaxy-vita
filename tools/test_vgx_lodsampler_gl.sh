#!/usr/bin/env bash
# GXV_LODBIAS_SAMPLER (lodmask37) on the production pipeline and Mesa (tests/vgx_lodsampler_gl.cpp; no game data): the picture with the bias in the sampler
# (switch 1) must equal the picture with the bias in the shader (switch 0), pixel for pixel. Needs EGL / GLES2 headers and Mesa (llvmpipe).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
units=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync
       vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${units[@]}"; do args+=("$src/$unit.cpp"); done
"${CXX:-clang++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_lodsampler_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
  -lEGL -lGLESv2 -pthread -o "$out/vgx_lodsampler_gl"
res=()
for sw in 0 1; do
  for split in 0 1; do
    line=$(env GXV_FIX_LODBIAS=1 GXV_LODBIAS_SAMPLER="$sw" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_SPLIT="$split" GXV_FXHALF=0 \
      GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 "$out/vgx_lodsampler_gl" "$sw" 2>&1 | grep -E '^(RESULT|FAIL)' )
    echo "switch $sw split $split: $line"
    res+=("$line")
  done
done
for r in "${res[@]}"; do [[ "$r" == "${res[0]}" ]] || { echo "FAIL: the sampler's picture differs from the shader's"; exit 1; }; done
[[ "${res[0]}" == RESULT* ]] || { echo "FAIL: no result"; exit 1; }
echo "PASS: identical pixels, switch 0 and 1, split 0 and 1"

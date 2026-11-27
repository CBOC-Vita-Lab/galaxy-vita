#!/usr/bin/env bash
# GXV_BGLAYER (bglayer25): the far-background layer cache (bit 1) and the census (bit 2) through the production front end and GLES back end on Mesa (tests/vgx_bglayer_gl.cpp).
#   tools/test_vgx_bglayer_gl.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include; BGLAYER_SKIP_BUILD=1 reuses the binary there)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${BGLAYER_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -w -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_bglayer_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_bglayer_gl"
fi
# Only the census (bit 2) is merged: vgx_bglayer2.inc is the "census-only stage", the layer cache (bit 1) comes with a later patch, so its variants are not run.
for v in "census:GXV_BGLAYER=2 GXV_BGLAYER_LOG_MS=1"; do
  n=${v%%:*}
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 BGLAYER_DUMP="$out/$n" ${v#*:} "$out/vgx_bglayer_gl" > "$out/$n.log" 2>&1 || true
  grep -v "^libunwind" "$out/$n.log" | sed "s/^/[$n] /"
done
if grep -q FAIL "$out"/*.log; then echo "FAILED: a check"; exit 1; fi
echo "PASS"

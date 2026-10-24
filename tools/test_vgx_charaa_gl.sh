#!/usr/bin/env bash
# GL-level test of the characters' edge smoothing at the present (GXV_CHARAA, tests/vgx_charaa_gl_test.cpp): the real vgx
# back end on the desktop's GL (Mesa llvmpipe through surfaceless EGL) presents a synthetic edge with and without a
# character rectangle; outside it the picture must be unchanged bit for bit, flat parts inside it within 1/255, the edge
# smoothed, and no scene added. Writes before / after crops (PPM, 4x) into the output directory.
#   tools/test_vgx_charaa_gl.sh [output-dir]
# Needs libEGL / libGLESv2 (Mesa). Exit status 0 when every check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_charaa_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/vgx_charaa_gl_test"
GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_CHARAA=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 "$out/vgx_charaa_gl_test" "$out"

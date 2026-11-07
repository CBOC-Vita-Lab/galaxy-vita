#!/usr/bin/env bash
# GL-level test of the whole-picture edge smoothing at the present (GXV_EDGE_AA, tests/vgx_edgeaa_gl_test.cpp): the real vgx back end
# on the desktop's GL (Mesa llvmpipe through surfaceless EGL) presents a synthetic edge with and without the pass, at 960x544 (1:1)
# and 640x456 (upscaled), with one HUD rectangle that must stay bit for bit. Writes before / after crops (PPM, 4x) into the output
# directory.
#   tools/test_vgx_edgeaa_gl.sh [output-dir]
# Needs libEGL / libGLESv2 (Mesa). Exit status 0 when every check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_edgeaa_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/vgx_edgeaa_gl_test"
GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_EDGE_AA=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 "$out/vgx_edgeaa_gl_test" "$out"
# GXV_EDGE_AA=3: the left half of the screen only (the console's same-picture A/B)
GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_EDGE_AA=3 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 "$out/vgx_edgeaa_gl_test" "$out"

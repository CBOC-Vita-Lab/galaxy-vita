#!/usr/bin/env bash
# GL-level test of a change of the back end's render scale between frames (tests/vgx_autoq_gl_test.cpp): the real
# vgx back end on the desktop's GL (Mesa llvmpipe through surfaceless EGL) executes synthetic command buffers, the rung
# is changed between frames, and the resampled render textures, atlas cells and EFB are checked. Run twice: GXV_KEEP_RT=0
# (the console's way: old textures and objects deleted) and =1 (the emulator's: forgotten, never deleted).
#   tools/test_vgx_autoq_gl.sh [output-dir]
# Needs libEGL / libGLESv2 (Mesa). Exit status 0 when every check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_autoq_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/vgx_autoq_gl_test"
status=0
# (The first pass also simulates 24 MiB of free video memory for the step-up check; the second has no figure.)
GXV_AUTOQ_VRAM_KIB=24576 GXV_KEEP_RT=0 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 "$out/vgx_autoq_gl_test" || status=1
GXV_KEEP_RT=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 "$out/vgx_autoq_gl_test" || status=1
exit $status

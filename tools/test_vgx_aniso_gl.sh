#!/usr/bin/env bash
# Anisotropic filtering as a run-time step (GXV_AUTOQ_ANISO) on the real GLES back end and Mesa: the switch hook between frames and the
# one-time re-setting of the trilinear textures' minification filter after a change (tests/vgx_aniso_gl_test.cpp). No game data.
# Needs the Mesa EGL/GLES host libraries. Exit status 0 when every check passes.
#   tools/test_vgx_aniso_gl.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_aniso_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -Wl,--wrap=glTexParameteri -lEGL -lGLESv2 -lpthread -o "$out/vgx_aniso_gl_test"
env -i PATH="$PATH" HOME="${HOME:-/tmp}" LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_FXHALF_MIN=0 GXV_WORKLOAD_PROBE=0 GXV_AUTOQ=0 GXV_KEEP_RT=0 \
  "$out/vgx_aniso_gl_test" 2>&1 | grep -v libunwind
exit "${PIPESTATUS[0]}"

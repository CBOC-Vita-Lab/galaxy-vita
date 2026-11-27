#!/usr/bin/env bash
# Linux/Mesa: identical existing pixel checks and glFinish/readback counts with the observer off/on.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-clang++}" -std=c++20 -O1 -g -DPORT_RUNTIME_DIAGNOSTICS=0 \
  -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_workload_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread \
  -Wl,--wrap=glFinish,--wrap=glReadPixels -o "$out/workload-gl"
for mode in off on; do
  args=()
  if test "$mode" = on; then args+=(on); fi
  GXV_AUTOQ_VRAM_KIB=24576 GXV_KEEP_RT=0 EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 \
    "$out/workload-gl" "${args[@]}" > "$out/$mode.log" 2>&1
done
off=$(grep '^workload GL calls:' "$out/off.log")
on=$(grep '^workload GL calls:' "$out/on.log")
test "$off" = "$on"
printf '%s\n' "$on"
grep '^PASS: real backend' "$out/off.log" "$out/on.log"
printf '%s\n' 'PASS: workload probe adds no GL finish/readback calls'

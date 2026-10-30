#!/usr/bin/env bash
# Real backend, real Mesa GLES pixels; only immediate texture-name reuse is injected.
# Pass --require-fixed after output-dir to demonstrate RED against unmodified source.
# SANITIZE=1 enables ASan/UBSan; Mesa process-exit leak checking is disabled by default.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
extra=()
if [[ ${SANITIZE:-0} == 1 ]]; then
  extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
fi
"${CXX:-g++}" -std=gnu++20 -O1 -g -w "${extra[@]}" -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_efbswap_failure_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread \
  -Wl,--wrap=glGenTextures -Wl,--wrap=glDeleteTextures -Wl,--wrap=glTexImage2D -Wl,--wrap=glGetError -o "$out/efbswap-failure"
export GXV_AUTOQ=0 GXV_AUTOQ_LEVEL=2 GXV_EFBSWAP=1 GXV_KEEP_RT=0 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0
if [[ ${2:-} == --require-fixed ]]; then
  GXV_EFB_REBIND_GUARD=1 "$out/efbswap-failure"
  exit
fi
env -u GXV_EFB_REBIND_GUARD "$out/efbswap-failure" --expect-legacy
GXV_EFB_REBIND_GUARD=0 "$out/efbswap-failure" --expect-legacy
GXV_EFB_REBIND_GUARD=1 "$out/efbswap-failure"
GXV_EFB_REBIND_GUARD=0 "$out/efbswap-failure" --expect-legacy --flip-after-first
GXV_EFB_REBIND_GUARD=1 "$out/efbswap-failure" --expect-fixed --flip-after-first

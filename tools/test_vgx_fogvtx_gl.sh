#!/usr/bin/env bash
# GXV_FOGVTX / GXV_NODISCARD: real generated GLES2 shaders on Mesa (pixel oracle), and byte identity of the generator
# with both switches off. No game data or Vita executable needed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_fogvtx_gl.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_fogvtx_gl"
run() { env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_fogvtx_gl" 2>&1 | grep -v '^libunwind'; }
# default / unset / invalid values are the off state; 1 = exact vertex fog (ortho linear) + no discard; 2 adds perspective linear
run -u GXV_FOGVTX -u GXV_NODISCARD
run GXV_FOGVTX=0 GXV_NODISCARD=0
run GXV_FOGVTX=bogus GXV_NODISCARD=bogus
run GXV_FOGVTX=1
run GXV_FOGVTX=2
run GXV_NODISCARD=1
run GXV_FOGVTX=2 GXV_NODISCARD=1

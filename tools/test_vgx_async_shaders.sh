#!/usr/bin/env bash
# GXV_ASYNC_SHADERS: the pure part (platform/vgx/src/vgx_async_shaders.h) with a fake compiler that takes time (tests/vgx_async_shaders.cpp),
# then the back end's use of it on the GL stub (tests/vgx_async_shaders_backend.cpp): once with the switch on and once with GXV_ASYNC_SHADERS=0.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_async_shaders.cpp" -o "$out/vgx_async_shaders" -lpthread
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_async_shaders"
"${CXX:-g++}" -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_async_shaders_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_async_shaders_backend" -lpthread
"$out/vgx_async_shaders_backend" on
"$out/vgx_async_shaders_backend" off

#!/usr/bin/env bash
# Real generated GLES2 shaders on Mesa; no game data or Vita executable needed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_tevwrap_gl.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_tevwrap_gl"
if [[ ${2:-} == copy-probe ]]; then
  GXV_TEVWRAP=1 LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevwrap_gl" on copy-probe
  exit
fi
if [[ ${2:-} == key-probe ]]; then
  GXV_TEVWRAP=1 LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevwrap_gl" on key-probe "$3" "$4"
  exit
fi
for value in unset 0 1 2 -1 invalid; do
  mode=off
  if [[ $value == unset ]]; then
    env -u GXV_TEVWRAP LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevwrap_gl" "$mode"
  else
    [[ $value != 1 ]] || mode=on
    GXV_TEVWRAP="$value" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevwrap_gl" "$mode"
  fi
done

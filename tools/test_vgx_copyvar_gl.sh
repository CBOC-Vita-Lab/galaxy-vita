#!/usr/bin/env bash
# GXV_COPYVAR: the dedicated copy programs against the generic one, rendered on Mesa's GLES2 (no game data, no Vita executable).
#   tools/test_vgx_copyvar_gl.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_copyvar_gl.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_copyvar_gl"
LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_copyvar_gl"

#!/usr/bin/env bash
# Pure host test of platform/vgx/src/vgx_sanitize.h, then the GLES test of the backend's use of it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/vgx/src" \
  "$root/tests/vgx_sanitize.cpp" -o "$out/vgx_sanitize"
"$out/vgx_sanitize"
bash "$root/tools/test_vgx_sanitize_gl.sh" "$out/gl"

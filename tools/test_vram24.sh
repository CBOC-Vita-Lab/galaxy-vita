#!/usr/bin/env bash
# vram24: host test of the video-memory rules (platform/vgx/src/vgx_vram24.h) and, where the back end builds on this host, a syntax check of the touched units.
#   tools/test_vram24.sh [output-dir]      exit status 0 when every check passes
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/platform/vgx/src" "$root/tests/vram24_test.cpp" -o "$out/vram24_test"
"$out/vram24_test"
khr=${KHR_INCLUDE:-/work/khr/include}
echo "== back end syntax (host)"
"${CXX:-g++}" -std=gnu++20 -O0 -w -fsyntax-only -I"$root/platform/vgx/src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" "$root/platform/vgx/src/vgx_backend_gl.cpp"
echo "ok: vram24"

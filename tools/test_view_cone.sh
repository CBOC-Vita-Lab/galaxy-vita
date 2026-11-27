#!/usr/bin/env bash
# GXV_FLUSH_OFFSCREEN (gamepar): host check of port/view_cone.h (tests/view_cone.cpp). bash tools/test_view_cone.sh [neg]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/view-cone.XXXXXX")
"${CXX:-g++}" -std=c++17 -O2 -I"$root/platform/include" "$root/tests/view_cone.cpp" -o "$out/view_cone"
"$out/view_cone" "$@"

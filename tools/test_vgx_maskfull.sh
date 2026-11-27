#!/usr/bin/env bash
# GXV_MASKFULL (lodmask37): the decision and the plan's alpha liveness (platform/vgx/src/vgx_maskfull.h; tests/vgx_maskfull_test.cpp). Pure: no GL, no game
# data. The back end's use of them (ApplyRenderState's mask, the "maskfull:" census line) needs the GLES back end and the console.
# Exit status 0 when every check passes.   tools/test_vgx_maskfull.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=undefined -I"$root/platform/vgx/src" "$root/tests/vgx_maskfull_test.cpp" -o "$out/vgx_maskfull_test"
"$out/vgx_maskfull_test"

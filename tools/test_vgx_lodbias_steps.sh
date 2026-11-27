#!/usr/bin/env bash
# The texture LOD bias sums and their clamp to vitaGL's -31 .. +32 (platform/vgx/src/vgx_lodbias.h; tests/vgx_lodbias_steps.cpp). Pure: no GL, no
# game data. The back end's use of them (GXV_RUNG_LODBIAS, _EXTRA, GXV_PLAYER_LODBIAS on the real GLES back end) is tools/test_vgx_sharp24_gl.sh and
# tools/test_vgx_mario24_gl.sh. Exit status 0 when every check passes.
#   tools/test_vgx_lodbias_steps.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=undefined -I"$root/platform/vgx/src" "$root/tests/vgx_lodbias_steps.cpp" -o "$out/vgx_lodbias_steps"
"$out/vgx_lodbias_steps"

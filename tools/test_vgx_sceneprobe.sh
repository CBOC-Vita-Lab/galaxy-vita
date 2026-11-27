#!/usr/bin/env bash
# GXV_SCENE_PROBE's pure part (platform/vgx/src/vgx_sceneprobe.h; tests/vgx_sceneprobe.cpp): no GL, no game data.
#   tools/test_vgx_sceneprobe.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=undefined -I"$root/platform/vgx/src" "$root/tests/vgx_sceneprobe.cpp" -o "$out/vgx_sceneprobe"
"$out/vgx_sceneprobe"

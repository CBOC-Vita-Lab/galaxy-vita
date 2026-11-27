#!/usr/bin/env bash
# GXV_GPU_CENSUS arithmetic (platform/vgx/src/vgx_gpucensus.h; tests/vgx_gpucensus.cpp): pure, no GL, no game data.
#   tools/test_vgx_gpucensus.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$root/platform/vgx/src" "$root/tests/vgx_gpucensus.cpp" -o "$out/vgx_gpucensus"
"$out/vgx_gpucensus"

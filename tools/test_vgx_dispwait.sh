#!/usr/bin/env bash
# The display scene's estimated wait for its buffer (platform/vgx/src/vgx_dispwait.h; tests/vgx_dispwait.cpp): pure, no GL, no game data.
#   tools/test_vgx_dispwait.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=undefined -I"$root/platform/vgx/src" "$root/tests/vgx_dispwait.cpp" -o "$out/vgx_dispwait"
"$out/vgx_dispwait"

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 -m unittest discover -s "$root/tests" -p test_loading_artwork.py
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/vita" "$root/tests/loading_artwork.cpp" -o "$out/check"
"$out/check" "$root/vita/vgx-game/sce_sys/loading-title.rgba" "$out"

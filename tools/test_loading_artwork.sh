#!/usr/bin/env bash
# The loading screen's picture comes from the player's own capture (ux0:data/smg/art/title.rgba) and falls back to a neutral
# starfield; the package carries no game artwork. Optional: LOADING_CAPTURE=<960x544 RGBA file> to run the capture cases on a real
# title capture instead of the synthetic one.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 -m unittest discover -s "$root/tests" -p test_loading_artwork.py
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/vita" "$root/tests/loading_artwork.cpp" -o "$out/check"
capture=-
if [ -n "${LOADING_CAPTURE:-}" ] && [ -f "$LOADING_CAPTURE" ]; then capture=$LOADING_CAPTURE; fi
"$out/check" "$capture" "$out"
"$out/check" - "$out"

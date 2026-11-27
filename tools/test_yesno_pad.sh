#!/usr/bin/env bash
# setfix31: the pad on the game's Yes / No window (platform/include/port/yesno_pad.h). CXX=g++ where clang has no ASan runtime.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/platform/include" "$root/tests/yesno_pad.cpp" -o "$out/test"
"$out/test"

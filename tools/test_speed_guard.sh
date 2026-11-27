#!/usr/bin/env bash
# GXV_SPEED_GUARD (port/speed_guard.h): host model of the pacing rules with the game-speed guard (tests/speed_guard_test.cpp prints the report's table).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror -include cmath -I"$root/platform/include" "$root/tests/speed_guard_test.cpp" -o "$out/t"
"$out/t"

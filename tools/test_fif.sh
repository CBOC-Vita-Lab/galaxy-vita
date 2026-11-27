#!/usr/bin/env bash
# GXV_FIF (port/fif.h): pipeline model of the display-end frames-in-flight gate.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror -include cmath -I"$root/platform/include" "$root/tests/fif_test.cpp" -o "$out/t"
"$out/t"

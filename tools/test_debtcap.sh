#!/usr/bin/env bash
# GXV_DEBT_CAP (frame_pace.h): the catch-up turns after a 33-80 ms turn, default against cap.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/debtcap_test.cpp" -o "$out/t"
"$out/t"

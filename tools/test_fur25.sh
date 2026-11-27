#!/bin/bash
# reflfur25: host test of platform/include/port/fur25.h (tests/fur25.cpp).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/fur25_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/fur25.cpp" -o "$out/fur25" || { echo "build FAILED"; exit 2; }
"$out/fur25"; rc=$?; rm -rf "$out"; exit $rc

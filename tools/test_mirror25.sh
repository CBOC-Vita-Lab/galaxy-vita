#!/bin/bash
# reflfur25: host test of platform/include/port/mirror25.h (tests/mirror25.cpp).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/mirror25_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -Wall -Wextra -I"$root/platform/include" "$root/tests/mirror25.cpp" -o "$out/mirror25" || { echo "build FAILED"; exit 2; }
"$out/mirror25"; rc=$?; rm -rf "$out"; exit $rc

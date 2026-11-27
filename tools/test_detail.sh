#!/bin/bash
# detail25: host test of platform/include/port/detail.h (tests/detail.cpp).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/detail_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -Wall -I"$root/platform/include" "$root/tests/detail.cpp" -o "$out/detail" || { echo "build FAILED"; exit 2; }
"$out/detail"; rc=$?; rm -rf "$out"; exit $rc

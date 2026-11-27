#!/bin/bash
# cliprange25: host test of platform/include/port/cliprange.h (tests/cliprange.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/cliprange_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/cliprange.cpp" -o "$out/cliprange" || { echo "build FAILED"; exit 2; }
"$out/cliprange"; rc=$?; rm -rf "$out"; exit $rc

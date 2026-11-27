#!/bin/bash
# far30: host test of platform/include/port/distcensus.h (tests/distcensus.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/distcensus_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/distcensus.cpp" -o "$out/distcensus" || { echo "build FAILED"; exit 2; }
"$out/distcensus"; rc=$?; rm -rf "$out"; exit $rc

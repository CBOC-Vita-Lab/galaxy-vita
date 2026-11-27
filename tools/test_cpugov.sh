#!/bin/bash
# honey25: host test of platform/include/port/cpugov.h (tests/cpugov.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/cpugov_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/cpugov.cpp" -o "$out/cpugov" || { echo "build FAILED"; exit 2; }
"$out/cpugov"; rc=$?; rm -rf "$out"; exit $rc

#!/bin/bash
# cpucuts25: host test of platform/include/port/cpucuts.h (tests/cpucuts.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/cpucuts_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/cpucuts.cpp" -o "$out/cpucuts" || { echo "build FAILED"; exit 2; }
"$out/cpucuts"; rc=$?; rm -rf "$out"; exit $rc

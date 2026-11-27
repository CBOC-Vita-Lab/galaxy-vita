#!/bin/bash
# cpulod26: host test of platform/include/port/cpulod.h (tests/cpulod.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/cpulod_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/cpulod.cpp" -o "$out/cpulod" || { echo "build FAILED"; exit 2; }
"$out/cpulod"; rc=$?; rm -rf "$out"; exit $rc

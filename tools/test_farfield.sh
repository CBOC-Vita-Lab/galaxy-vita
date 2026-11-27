#!/bin/bash
# far30: host test of platform/include/port/farfield.h (tests/farfield.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/farfield_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/farfield.cpp" -o "$out/farfield" || { echo "build FAILED"; exit 2; }
"$out/farfield"; rc=$?; rm -rf "$out"; exit $rc

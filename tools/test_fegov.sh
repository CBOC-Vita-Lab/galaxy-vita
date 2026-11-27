#!/bin/bash
# fegov26: host test of platform/include/port/fegov.h (tests/fegov.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/fegov_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/fegov.cpp" -o "$out/fegov" || { echo "build FAILED"; exit 2; }
"$out/fegov"; rc=$?; rm -rf "$out"; exit $rc

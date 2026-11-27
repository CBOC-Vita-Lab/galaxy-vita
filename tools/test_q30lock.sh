#!/bin/bash
# q30lock26: host test of platform/include/port/q30lock.h and lockfade.h (tests/q30lock_test.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/q30lock_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/q30lock_test.cpp" -o "$out/q30lock" || { echo "build FAILED"; exit 2; }
"$out/q30lock"; rc=$?; rm -rf "$out"; exit $rc

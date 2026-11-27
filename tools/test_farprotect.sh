#!/bin/bash
# farprotect35: host test of platform/include/port/farprotect.h (tests/farprotect.cpp).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/farprotect_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -I"$root/platform/include" "$root/tests/farprotect.cpp" -o "$out/t" || { echo "build FAILED"; exit 2; }
"$out/t"; rc=$?; rm -rf "$out"; exit $rc

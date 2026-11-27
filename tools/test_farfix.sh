#!/bin/bash
# farfix35: host test of platform/include/port/farfix.h (tests/farfix.cpp).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/farfix_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -I"$root/platform/include" "$root/tests/farfix.cpp" -o "$out/t" || { echo "build FAILED"; exit 2; }
"$out/t"; rc=$?; rm -rf "$out"; exit $rc

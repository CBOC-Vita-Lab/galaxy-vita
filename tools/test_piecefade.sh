#!/bin/bash
# q30fin26: host test of platform/include/port/piecefade.h (tests/piecefade_test.cpp), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/piecefade_test.$$"; mkdir -p "$out"
"${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/piecefade_test.cpp" -o "$out/piecefade" || { echo "build FAILED"; exit 2; }
"$out/piecefade"; rc=$?; rm -rf "$out"; exit $rc

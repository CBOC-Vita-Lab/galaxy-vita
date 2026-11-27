#!/bin/bash
# anticip32: host test of platform/include/port/anticipate.h, the Quality ladder's overlay (q30lock.h) and the rate controller's hook (rate_auto.h): tests/anticipate_test.cpp, ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/anticipate_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/anticipate_test.cpp" -o "$out/anticipate" || { echo "build FAILED"; exit 2; }
"$out/anticipate"; rc=$?; rm -rf "$out"; exit $rc

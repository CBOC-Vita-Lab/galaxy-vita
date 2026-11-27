#!/bin/bash
# ratecut31: host test of platform/include/port/pose_phase.h (tests/pose_phase.cpp: identity of mode 0, the turn / picture model, the flower members'
# clock, seven mutants), ASan + UBSan.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/pose_phase_test.$$"; mkdir -p "$out"
"${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/include" "$root/tests/pose_phase.cpp" -o "$out/pose_phase" || { echo "build FAILED"; rm -rf "$out"; exit 2; }
"$out/pose_phase"; rc=$?; rm -rf "$out"; exit $rc

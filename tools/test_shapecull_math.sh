#!/usr/bin/env bash
# cpu24: shapecull_math::FastPlane == RefPlane (the original plane loop) over random boxes. No FMA contraction: the console CPU (Cortex-A9, VFPv3) has no fused multiply-add.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
for fl in "-ffp-contract=off" "-O3 -ffp-contract=off -fno-fast-math"; do
    "${CXX:-g++}" -std=c++20 -O2 $fl -Wall -Wextra -I"$root/platform/include" "$root/tests/shapecull_math_test.cpp" -o "$out/scm"
    "$out/scm"
done
echo PASS

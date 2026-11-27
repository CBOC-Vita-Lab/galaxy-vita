#!/bin/bash
# hotprof25 round 2: GXV_HOTPROF_BOXCULL's box test (platform/vgx/src/vgx_hotprof_boxcull.inc HpBoxOutMask) against the test as it was
# (BoxOutMaskOld, same file): the same mask for every box (random, built on the planes / the margin, NaN / inf / denormal / huge numbers,
# perspective and orthographic), the negative control (must FAIL), an ASan + UBSan run, and the micro-benchmark (host time).
# `bash tools/test_vgx_hotprof_boxcull.sh [outdir] [boxes]`.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hotprof-boxcull.XXXXXX")}
n=${2:-4000000}
mkdir -p "$out"
cxx=${CXX:-clang++}
# (the console's floating-point rules: no fast-math, contraction "on"; x86-64 without FMA and the Vita's VFPv3 both have no fused multiply-add)
flags=(-std=c++20 -O2 -ffp-contract=on -fno-fast-math)
"$cxx" "${flags[@]}" "$root/tests/vgx_hotprof_boxcull.cpp" -o "$out/hp_boxcull"
"$out/hp_boxcull" "$n" bench
g++ -std=c++20 -O1 -g -fsanitize=address,undefined "$root/tests/vgx_hotprof_boxcull.cpp" -o "$out/hp_boxcull_asan"
"$out/hp_boxcull_asan" 300000 | tail -2
"$cxx" "${flags[@]}" -DHP_BOX_NEG=100.0f "$root/tests/vgx_hotprof_boxcull.cpp" -o "$out/hp_boxcull_neg"
if "$out/hp_boxcull_neg" "$n" > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(grep mismatches "$out/neg.txt" | tail -1)"; fi
echo "test_vgx_hotprof_boxcull: PASS"

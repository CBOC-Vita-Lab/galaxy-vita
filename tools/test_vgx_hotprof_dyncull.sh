#!/bin/bash
# hotprof25 (11 Nov): GXV_HOTPROF_DYNCULL's rigid, perspective test (platform/vgx/src/vgx_hotprof_dyncull.inc) against the loop over PointOutMask it
# replaces: the same answer for every draw (random, particle quads, built on the planes / the margins, NaN / inf / denormal / huge numbers), the
# negative control (must FAIL), an ASan + UBSan run, and the micro-benchmark (host time). `bash tools/test_vgx_hotprof_dyncull.sh [outdir] [draws]`.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hotprof-dyncull.XXXXXX")}
n=${2:-4000000}
mkdir -p "$out"
cxx=${CXX:-clang++}
# (the console's floating-point rules: no fast-math, contraction "on"; x86-64 without FMA and the Vita's VFPv3 both have no fused multiply-add)
flags=(-std=c++20 -O2 -ffp-contract=on -fno-fast-math)
"$cxx" "${flags[@]}" "$root/tests/vgx_hotprof_dyncull.cpp" -o "$out/hp_dyncull"
"$out/hp_dyncull" "$n" bench
g++ -std=c++20 -O1 -g -fsanitize=address,undefined "$root/tests/vgx_hotprof_dyncull.cpp" -o "$out/hp_dyncull_asan"
"$out/hp_dyncull_asan" 300000 | tail -2
"$cxx" "${flags[@]}" -DHP_NEG=1 "$root/tests/vgx_hotprof_dyncull.cpp" -o "$out/hp_dyncull_neg"
if "$out/hp_dyncull_neg" 400000 > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(tail -2 "$out/neg.txt" | head -1)"; fi
"$cxx" "${flags[@]}" -DHP_BOUND_FACTOR=0.25f "$root/tests/vgx_hotprof_dyncull.cpp" -o "$out/hp_dyncull_neg2"
if "$out/hp_dyncull_neg2" 2000000 > "$out/neg2.txt"; then echo "second negative control (a bound that is none) PASSED (it must fail): FAIL"; exit 1; else echo "second negative control fails as it must: $(grep mismatches "$out/neg2.txt" | tail -1)"; fi
echo "test_vgx_hotprof_dyncull: PASS"

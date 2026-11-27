#!/bin/bash
# update25 (11 Nov): host test of port/live_set.h (GXV_UPD_LIVESET's table) against the binary search. `bash tools/test_live_set.sh [outdir]`.
# Prints PASS or FAIL; also runs the negative control (must FAIL) and the ASan + UBSan build.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/live-set.XXXXXX")}
mkdir -p "$out"
cxx=${CXX:-clang++}
"$cxx" -std=c++20 -O2 -I"$root/platform/include" "$root/tests/live_set.cpp" -o "$out/live_set"
"$out/live_set"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$root/platform/include" "$root/tests/live_set.cpp" -o "$out/live_set_asan"
"$out/live_set_asan"
"$cxx" -std=c++20 -O2 -DPORT_LIVESET_NEG=1 -I"$root/platform/include" "$root/tests/live_set.cpp" -o "$out/live_set_neg"
if "$out/live_set_neg" > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(tail -2 "$out/neg.txt" | head -1)"; fi

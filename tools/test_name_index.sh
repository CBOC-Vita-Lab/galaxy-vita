#!/bin/bash
# update25 (11 Nov): host test of port/name_index.h (GXV_UPD_SNDNAME's index) against the scan. `bash tools/test_name_index.sh [outdir]`.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/name-index.XXXXXX")}
mkdir -p "$out"
cxx=${CXX:-clang++}
"$cxx" -std=c++20 -O2 -I"$root/platform/include" "$root/tests/name_index.cpp" -o "$out/name_index"
"$out/name_index"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$root/platform/include" "$root/tests/name_index.cpp" -o "$out/name_index_asan"
"$out/name_index_asan"
"$cxx" -std=c++20 -O2 -DPORT_NAMEIDX_NEG=1 -I"$root/platform/include" "$root/tests/name_index.cpp" -o "$out/name_index_neg"
if "$out/name_index_neg" > "$out/neg.txt"; then echo "negative control PASSED (it must fail): FAIL"; exit 1; else echo "negative control fails as it must: $(tail -2 "$out/neg.txt" | head -1)"; fi

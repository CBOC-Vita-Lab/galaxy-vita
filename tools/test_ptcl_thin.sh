#!/usr/bin/env bash
# GXV_PTCL_THIN host test (gamecpu25). PORT_THIN_NEG=1 is the negative control (index selection: must FAIL).   bash tools/test_ptcl_thin.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -g -fsanitize=address,undefined -I"$root/platform/include" "$root/tests/ptcl_thin.cpp" -o "$out/ptcl_thin"
ASAN_OPTIONS=detect_leaks=0 "$out/ptcl_thin"
if PORT_THIN_NEG=1 ASAN_OPTIONS=detect_leaks=0 "$out/ptcl_thin" > "$out/neg.txt" 2>&1; then echo "negative control did not fail"; cat "$out/neg.txt"; exit 1; fi
echo "negative control fails as it must: $(grep -m1 FAIL "$out/neg.txt")"

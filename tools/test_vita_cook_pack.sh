#!/usr/bin/env bash
# cook24: host test of the user shader pack (round trip, truncation / flip refusal, ABI, A/B slots, LRU, bounds), ASan + UBSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/vita_cook_pack_$$
mkdir -p "$out"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/vgx/third_party" \
  "$root/tests/vita_cook_pack.cpp" -o "$out/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test" "$out/data"

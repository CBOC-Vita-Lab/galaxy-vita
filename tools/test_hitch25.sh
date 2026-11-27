#!/usr/bin/env bash
# hitch25: host tests of the per-stage "hitch25:" line's pure parts and the sliced stage tally (GXV_STAGEBUILD_SLICE), plain and ASan+UBSan.  bash tools/test_hitch25.sh [out-dir]
# (also runs the smooth25 test: the tally cache's per-target pass now walks a candidate list)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hitch25.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/include" "$root/tests/hitch25_model.cpp" -o "$out/hitch25_model"
    "$out/hitch25_model"
done
bash "$root/tools/test_smooth25.sh" "$out/smooth25"

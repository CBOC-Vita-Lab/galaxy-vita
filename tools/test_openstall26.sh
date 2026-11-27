#!/usr/bin/env bash
# openstall26: host test of GXV_SMOOTHSTART's pure parts and the stage tally's SelectTop (GXV_STAGEBUILD_FAST), plain and ASan+UBSan.  bash tools/test_openstall26.sh [out-dir]
# (also runs the hitch25 test, which holds the sliced tally's tests: MakeWhole was split out of Select)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/openstall26.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/include" "$root/tests/openstall26_model.cpp" -o "$out/openstall26_model"
    "$out/openstall26_model"
done
bash "$root/tools/test_hitch25.sh" "$out/hitch25"

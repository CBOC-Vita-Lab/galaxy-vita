#!/usr/bin/env bash
# smooth25: host tests (classifier fields, PaceMetric, stage tally cache, fixed 30 pass-on, present slots), plain and ASan+UBSan.  bash tools/test_smooth25.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/smooth25.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/include" "$root/tests/smooth25_model.cpp" -o "$out/smooth25_model"
    "$out/smooth25_model"
done

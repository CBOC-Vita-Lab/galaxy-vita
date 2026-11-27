#!/usr/bin/env bash
# hitch24b: host tests of GXV_HITCH_LOG's classifier and journal, plain, ASan+UBSan.  bash tools/test_hitch24.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hitch24.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-clang++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/include" "$root/tests/hitch24_model.cpp" -o "$out/hitch24_model"
    "$out/hitch24_model"
done

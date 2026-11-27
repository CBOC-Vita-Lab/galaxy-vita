#!/usr/bin/env bash
# midload26: host test of vgx_midhold.h (GXV_NO_MIDHOLD's verdict), plain and ASan+UBSan.  bash tools/test_midhold.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/midhold.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/midhold_model.cpp" -o "$out/midhold_model"
    "$out/midhold_model"
done

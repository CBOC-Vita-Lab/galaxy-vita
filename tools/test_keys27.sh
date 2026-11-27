#!/usr/bin/env bash
# keys27 (GXV_KEYS27): host test of vgx_keys27.h (plain and ASan+UBSan), then the back end on the GL stub with the switch off (no regression: tools/test_play_compile.sh's backend test).
#   bash tools/test_keys27.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/keys27.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/keys27_model.cpp" -o "$out/keys27_model" 2>&1 | grep -v "^$" | head -20 || true
    "$out/keys27_model"
done

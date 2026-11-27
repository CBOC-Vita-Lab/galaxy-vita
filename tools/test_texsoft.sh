#!/usr/bin/env bash
# texup27: host test of GXV_TEXSOFT_FAST (platform/vgx/src/vgx_texsoft.h) against the old softness code, plain and ASan+UBSan.  bash tools/test_texsoft.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/texsoft.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/texsoft_model.cpp" -o "$out/texsoft_model"
    "$out/texsoft_model"
done

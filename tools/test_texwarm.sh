#!/usr/bin/env bash
# endwait26: host test of GXV_TEXWARM record / budget logic (platform/vgx/src/vgx_texwarm.h), plain and ASan+UBSan.  bash tools/test_texwarm.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/texwarm.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/texwarm_model.cpp" -o "$out/texwarm_model"
    "$out/texwarm_model"
done

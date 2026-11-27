#!/usr/bin/env bash
# menutrim25: host test of vgx_menu_split.h (GXV_STAGEQ_MENUSPLIT), plain and ASan+UBSan.  bash tools/test_menusplit.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/menusplit.XXXXXX")}
mkdir -p "$out"
for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
    "${CXX:-g++}" -std=gnu++17 -O1 -g -Wall -Wextra $san "$root/tests/menusplit_model.cpp" -o "$out/menusplit_model"
    "$out/menusplit_model"
done

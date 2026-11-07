#!/usr/bin/env bash
# GXV_LC_PANEMEMO host proof: port/pane_memo.h on random pane trees (tests/lc_pane_memo.cpp).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -fno-strict-aliasing -I"$root/platform/include" "$root/tests/lc_pane_memo.cpp" -o "$out/lc_pane_memo"
"$out/lc_pane_memo"

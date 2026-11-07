#!/usr/bin/env bash
# GXV_LC_KEYSEARCH host proof: port/key_search.h against the original key search (tests/lc_keysearch.cpp).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -fwrapv -fno-strict-aliasing -I"$root/platform/include" "$root/tests/lc_keysearch.cpp" -o "$out/lc_keysearch"
"$out/lc_keysearch" "$@"

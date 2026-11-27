#!/usr/bin/env bash
# GXV_EFBSWAP2_BACKOFF hold length (tests/efbswap_backoff_test.cpp). Host g++ only.   tools/test_efbswap_backoff.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/platform/vgx/src" "$root/tests/efbswap_backoff_test.cpp" -o "$out/efbswap_backoff_test"
"$out/efbswap_backoff_test"
echo "PASS: efbswap_backoff_test"

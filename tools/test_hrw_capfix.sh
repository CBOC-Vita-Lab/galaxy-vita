#!/usr/bin/env bash
# GXV_HRW_CAPFIX: the half-rate reuse rule with / without 60 Assist pairs (tests/hrw_capfix_test.cpp). Host g++ only, no GL, no game data.
#   tools/test_hrw_capfix.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -Wall "$root/tests/hrw_capfix_test.cpp" -o "$out/hrw_capfix_test"
"$out/hrw_capfix_test"
echo "PASS: hrw_capfix_test"

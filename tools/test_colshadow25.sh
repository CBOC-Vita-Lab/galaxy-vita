#!/bin/bash
# Host test of Mario's drop-shadow triangle selection (shadow25, GXV_COLSHADOW_STABLE). No GL, no game data. Usage: tools/test_colshadow25.sh
set -e
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/colshadow25_test.$$
g++ -std=c++17 -O1 -Wall -I platform/include tests/colshadow_select_test.cpp -o "$out"
"$out"
rm -f "$out"

#!/bin/bash
# Host test of the post-load settle gate for the size change's skip-resample evidence (bugs26, GXV_RUNG_SKIPRS_SETTLE). Usage: tools/test_rung_settle.sh
set -e
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/rung_settle_test.$$
g++ -std=c++17 -O1 -Wall -I platform/vgx/src tests/rung_settle_test.cpp -o "$out"
"$out"
rm -f "$out"

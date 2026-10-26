#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if test "$#" != 2 || test "$1" != --vitagl-source; then
    echo 'usage: test_vitagl_no_present.sh --vitagl-source <patched vitaGL root or source directory>' >&2
    exit 2
fi
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
node "$root/tools/prepare_vitagl_no_present_fixture.cjs" "$2" "$fixture/vitagl_no_present_functions.inc"
for timers in 0 1; do
    extra=(-DNO_PRESENT_FIXTURE=1)
    if test "$timers" = 1; then extra+=(-DGPU_TIMERS); fi
    "${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        "${extra[@]}" -I"$fixture" "$root/tests/vitagl_no_present_test.cpp" -o "$fixture/test"
    "$fixture/test"
done

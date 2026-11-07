#!/usr/bin/env bash
# Host test of vitaGL's per-picture data pool as the Galaxy back end uses it with HAVE_SCRATCH_MEMORY and GXV_VITA_SCRATCH=1 (stream buffers
# in the pool): the real driver text (pool, buffer functions, swap rotation) against models of the heap, the collector and the display queue.
# The nhustdx source set is a vitaGL tree with patches 0001-0012 applied (what tools/vitagl/build-release.sh prepares).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if test "$#" != 2 || test "$1" != --vitagl-source; then
    echo 'usage: test_vitagl_scratch_pool.sh --vitagl-source <patched vitaGL root or source directory>' >&2
    exit 2
fi
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
node "$root/tools/prepare_vitagl_scratch_fixture.cjs" "$2" "$fixture/vitagl_scratch_pool_functions.inc"
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-writable-strings -fpermissive \
    -I"$fixture" "$root/tests/vitagl_scratch_pool_test.cpp" -o "$fixture/test"
"$fixture/test"

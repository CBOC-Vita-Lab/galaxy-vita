#!/usr/bin/env bash
# Host tests of the patched vitaGL driver text: checked scene entry and depth/stencil creation (patches 0009, 0010) and, when the
# tree has it, the GXM state cache (patch 0011): the same random script of draws, internal clears and blits, texture and program
# changes and frame ends is run with and without GXM_STATE_CACHE, and the state the context would draw with, at every draw, must be
# identical (the script is run for a context that keeps its state across scene begins and for one that forgets it).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if test "$#" != 2 || test "$1" != --vitagl-source; then
    echo 'usage: test_vitagl_scene_fail.sh --vitagl-source <patched vitaGL root or source directory>' >&2
    exit 2
fi
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
node "$root/tools/prepare_vitagl_scene_fixture.cjs" "$2" "$fixture/vitagl_scene_functions.inc"
has_cache=0
if grep -q 'FIXTURE_HAS_SC 1' "$fixture/vitagl_scene_functions_config.h"; then has_cache=1; fi
for timers in 0 1; do
    extra=()
    if test "$timers" = 1; then extra+=(-DGPU_TIMERS); fi
    for cache in 0 1; do
        if test "$cache" = 1 && test "$has_cache" = 0; then continue; fi
        cextra=()
        if test "$cache" = 1; then cextra+=(-DGXM_STATE_CACHE); fi
        bin="$fixture/test_t${timers}_c${cache}"
        "${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-writable-strings -fpermissive \
            "${extra[@]}" "${cextra[@]}" -I"$fixture" "$root/tests/vitagl_scene_fail_test.cpp" -o "$bin"
        "$bin" "$fixture/trace_t${timers}_c${cache}.txt"
    done
    if test "$has_cache" = 1; then
        diff <(grep -v '^calls' "$fixture/trace_t${timers}_c0.txt") <(grep -v '^calls' "$fixture/trace_t${timers}_c1.txt")
        echo "PASS: GXM state cache: effective state at every draw identical with and without the cache (GPU_TIMERS=$timers)"
    fi
done

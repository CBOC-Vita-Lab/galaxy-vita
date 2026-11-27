#!/usr/bin/env bash
# Host test of the movie pipeline's pacing (movie36: GXV_THP_DELAY, the workers' order, the picture a call shows, the
# re-tiling of the hardware decoder's output; tests/thp_pacing.cpp). Needs only a C++ compiler; no game data, no libjpeg.
# The pipeline part runs twice: with the delay line (3 calls) and with the rules before it (0).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/thp-pacing.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    "$root/tests/thp_pacing.cpp" -o "$out/thp-pacing"
PETARI_THP_WORKERS=2 GXV_THP_DELAY=3 "$out/thp-pacing"
PETARI_THP_WORKERS=2 GXV_THP_DELAY=0 "$out/thp-pacing"

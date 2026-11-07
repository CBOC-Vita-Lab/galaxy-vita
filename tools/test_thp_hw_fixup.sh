#!/usr/bin/env bash
# Host test of the THP -> standard JPEG fix-up used by the hardware movie path (GXV_THPHW). Needs libjpeg (the same
# library tools/thp_transcode.cpp uses); no game data.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/thp-hw-fixup.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    "$root/tests/thp_hw_fixup.cpp" -o "$out/thp-hw-fixup" -ljpeg
"$out/thp-hw-fixup"

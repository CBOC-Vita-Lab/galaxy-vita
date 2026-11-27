#!/usr/bin/env bash
# stagepreset25: host test of the per-stage presets (tests/vgx_stagepreset_test.cpp): the parser and class resolution, the stage-settable switch registry
# and the controller's preset frame, under ASan + UBSan. Exit status 0 when every check passes.
#   tools/test_vgx_stagepreset.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_stagepreset_test.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_stagepreset_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_stagepreset_test"

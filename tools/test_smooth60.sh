#!/usr/bin/env bash
# policy26: host test of GXV_SMOOTH60 (tests/smooth60_test.cpp): the ladder's decisions (platform/include/port/smooth60.h), the stage table's three modes
# (ResolveMode in platform/vgx/src/vgx_stagepreset.h) and the steady 30 forced from outside (frame_pace.h), under ASan + UBSan. An optional second argument is a preset
# file (e.g. stage-presets-1112-smooth60.txt) whose every stage row is then checked in all three modes. Exit status 0 when every check passes.
#   tools/test_smooth60.sh [output-dir [preset-file]]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/smooth60_test.cpp" -o "$out/smooth60_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/smooth60_test" ${2:+"$2"}

#!/usr/bin/env bash
# GXV_PRESENT_SYNC / GXV_SKIP_PATTERN on a model of the console's pipeline (tests/present_sync_model.cpp; a model, not a console
# measurement): the unit checks of frame_pace.h TurnClock / PresentSync / SkipPattern, then the judder table.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/present_sync_model.cpp" -o "$out/present_sync_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/present_sync_model"

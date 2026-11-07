#!/usr/bin/env bash
# GXV_SKIP_MAXSTEP on a model of the lit Observatory's pipeline (tests/skip_maxstep_model.cpp; a model, not a console measurement):
# unit checks of frame_pace.h LegacySkip's step cap, then the table of pictures a second, game speed, turns per picture and gaps.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/skip_maxstep_model.cpp" -o "$out/skip_maxstep_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/skip_maxstep_model"

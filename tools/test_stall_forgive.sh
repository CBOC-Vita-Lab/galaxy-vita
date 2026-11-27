#!/usr/bin/env bash
# GXV_STALL_FORGIVE (frame_pace.h): a model of the game thread's loop after one long turn around the real LegacySkip / Controller
# (tests/stall_forgive_model.cpp; a model, not a console measurement): the table of what happens after 40 ms .. 2.4 s stalls with the switch
# off and on, and the checks (at most one turn without a picture after a stall with the switch; identical decisions without a stall).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/stall_forgive_model.cpp" -o "$out/stall_forgive_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/stall_forgive_model"

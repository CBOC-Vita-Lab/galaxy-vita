#!/usr/bin/env bash
# GXV_STEADY30, Performance mode's steady 30 (frame_pace.h SteadyCadence), on a model of the console's pipeline
# (tests/steady_cadence_model.cpp; a model, not a console measurement). Exit status 0 when every check passes.
#   tools/test_steady_cadence_model.sh [output-dir]      (default: a temporary directory)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/steady_cadence_model.cpp" -o "$out/steady_cadence_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/steady_cadence_model"

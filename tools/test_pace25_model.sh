#!/usr/bin/env bash
# GXV_PACE25 (platform/include/port/pace25.h) on a model of the console pipeline (tests/pace25_model.cpp; a model, not a console measurement): unit checks of the
# rule, then the legacy rule against the c69 console histograms, the legacy rule against GXV_PACE25=1 per stage, the existing parameters, and a sweep of the GPU's
# work per picture. perf.cpp's host path is syntax-checked. PACE25_FIT=1: fit the GPU's occupancy to the console's pictures a second first (default: the logged costs as they are).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/pace25_model.cpp" -o "$out/pace25_model"
UBSAN_OPTIONS=halt_on_error=1 "$out/pace25_model"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -fsyntax-only -DTARGET_PC -I"$root/platform/include" -I"$root/platform/src" -I"$root/platform" "$root/platform/src/port/perf.cpp"
echo "perf.cpp host path: syntax ok"

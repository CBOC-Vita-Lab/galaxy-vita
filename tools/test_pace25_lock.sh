#!/usr/bin/env bash
# pace25: the 48.0 fps lock at L 5 and GXV_PENDING_HOLD25 (frame_pace.h PresentSync::HoldAllowance) on an event model of the whole pacing loop (the real LegacySkip + PresentSync).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/pace25.XXXXXX")}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -Werror -I"$root/platform/include" "$root/tests/pace25_lock_model.cpp" -o "$out/lock"
"$out/lock"

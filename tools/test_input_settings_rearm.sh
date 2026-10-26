#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$root/tools/extract_tilt_consumer_fixture.py" "$out/tilt_consumer_actual.inc"
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -DPORT_RUNTIME_DIAGNOSTICS=0 \
  -DTARGET_PC -I"$out" -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" \
  "$root/tests/input_settings_rearm.cpp" "$root/tests/tilt_consumers.cpp" "$root/platform/src/input/wpad.cpp" -o "$out/test"
"$out/test"

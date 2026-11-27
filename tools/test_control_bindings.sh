#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread -I"$root/platform/include" \
  "$root/tests/control_bindings.cpp" "$root/platform/src/input/control_bindings.cpp" -o "$out/test"
ASAN_OPTIONS=detect_leaks=0 "$out/test"   # (settingsnative: StageActiveMapping's copies are never freed on purpose)
GXV_PAD_DEFAULTS=1 ASAN_OPTIONS=detect_leaks=0 "$out/test"

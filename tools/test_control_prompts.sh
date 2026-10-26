#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$root/platform/include" \
  "$root/tests/control_prompts.cpp" "$root/platform/src/input/control_bindings.cpp" -o "$out/test"
"$out/test" off
"$out/test" on

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/platform/include" \
  "$root/tests/pointer_aim.cpp" -o "$out/test"
"$out/test"

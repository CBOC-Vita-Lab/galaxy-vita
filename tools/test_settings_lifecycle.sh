#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/settings-lifecycle}"
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/include \
  tests/settings_lifecycle.cpp -o "$out/test"
"$out/test"

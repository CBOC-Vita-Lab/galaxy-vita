#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/settings-audio}"
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/include \
  -Itests/settings_stubs tests/settings_audio.cpp -o "$out/test"
"$out/test"

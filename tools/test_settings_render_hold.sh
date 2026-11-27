#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/settings-render}"
mkdir -p "$out"
python3 tools/prepare_settings_render_fixture.py "$out/settings_render_actual.inc"
"${CXX:-clang++}" -std=c++20 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iplatform/include -I"$out" \
  tests/settings_render_hold.cpp -o "$out/test"
"$out/test"

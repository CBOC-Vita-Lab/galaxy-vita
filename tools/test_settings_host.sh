#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/settings-host}"
mkdir -p "$out"
python3 tools/prepare_settings_host_fixture.py "$out/settings_host_actual.inc"
"${CXX:-clang++}" -std=c++20 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$out" tests/settings_host.cpp -o "$out/test"
"$out/test"

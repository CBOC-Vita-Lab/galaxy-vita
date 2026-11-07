#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/settings-save-guard}"
mkdir -p "$out"
python3 tools/prepare_settings_save_fixture.py "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iplatform/include -Iplatform/vgx/src -I"$out" \
  tests/settings_save_guard.cpp -o "$out/test"
for mode in disabled unset zero invalid on read-once-on read-once-off failed-save; do
  "$out/test" "$mode"
done

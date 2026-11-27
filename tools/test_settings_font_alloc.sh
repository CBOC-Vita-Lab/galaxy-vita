#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests
"${CXX:-clang++}" -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
  -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/include \
  tests/settings_font_allocation.cpp platform/src/settings/settings_font.cpp \
  -o local/settings-tests/font-allocation
local/settings-tests/font-allocation

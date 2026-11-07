#!/usr/bin/env bash
# Frame time of the Settings menu on the host at -O2 (see tests/settings_render_bench.cpp). SETTINGS_FONT_ARCHIVE=<Font.arc> adds the game fonts.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests
CXX=${CXX:-clang++}
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -Iplatform/include platform/src/settings/player_settings.cpp platform/src/settings/settings_store.cpp platform/src/input/control_bindings.cpp \
  platform/src/settings/settings_font.cpp platform/src/settings/settings_ui.cpp platform/src/settings/settings_draw.cpp tests/settings_render_bench.cpp -o local/settings-tests/render_bench
local/settings-tests/render_bench

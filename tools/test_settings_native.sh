#!/usr/bin/env bash
# Host test of the native Settings model (platform/src/settings/settings_native.cpp). Run under the queue: tools/buildrun.sh bash tools/test_settings_native.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-local/settings-tests}
mkdir -p "$out"
CXX=${CXX:-clang++}
flags=(-std=c++17 -Wall -Wextra -Werror -Wno-misleading-indentation -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/include -Itests)
srcs=(platform/src/settings/player_settings.cpp platform/src/settings/settings_store.cpp platform/src/input/control_bindings.cpp
      platform/src/settings/settings_native.cpp platform/src/settings/settings_ui.cpp platform/src/settings/settings_font.cpp platform/src/settings/settings_draw.cpp)
"$CXX" "${flags[@]}" "${srcs[@]}" tests/settings_native.cpp -o "$out/settings_native"
GXV_PAD_DEFAULTS=0 "$out/settings_native"
GXV_PAD_DEFAULTS=1 "$out/settings_native"

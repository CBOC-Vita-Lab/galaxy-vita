#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests local/settings-preview
CXX=${CXX:-clang++}
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Iplatform/include)
core=(platform/src/settings/player_settings.cpp platform/src/settings/settings_store.cpp platform/src/input/control_bindings.cpp)
run_test() {
  "$CXX" "${flags[@]}" "${core[@]}" "${@:2}" "tests/player_settings_$1.cpp" -o "local/settings-tests/$1"
  "local/settings-tests/$1"
}
case "${1:-all}" in
  config|store) run_test "$1" ;;
  launch) run_test launch platform/src/settings/settings_launch.cpp ;;
  font) run_test font platform/src/settings/settings_font.cpp ;;
  ui) run_test ui platform/src/settings/settings_font.cpp platform/src/settings/settings_ui.cpp platform/src/settings/settings_draw.cpp ;;
  preview)
    "$CXX" "${flags[@]}" "${core[@]}" platform/src/settings/settings_font.cpp platform/src/settings/settings_ui.cpp platform/src/settings/settings_draw.cpp tools/preview_player_settings.cpp -o local/settings-tests/preview
    output=${PREVIEW_OUT:-local/settings-preview}
    mkdir -p "$output"
    args=("$output")
    if [[ -n ${SETTINGS_FONT_ARCHIVE:-} ]]; then args+=("$SETTINGS_FONT_ARCHIVE"); fi
    local/settings-tests/preview "${args[@]}"
    python3 tools/handheld/settings_preview_png.py "$output" ;;
  all) bash "$0" config; bash "$0" store; bash "$0" launch; bash "$0" font; bash "$0" ui; bash "$0" preview ;;
  *) echo 'usage: test_player_settings.sh [config|store|launch|ui|preview|all]' >&2; exit 2 ;;
esac

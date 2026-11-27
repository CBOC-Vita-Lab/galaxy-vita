#!/bin/bash
# The menus' host tests (flow split of ui_menu_test.cpp): every screen family's tests, the fake port, the real menu code. Run from anywhere.
#   bash tests/mp/ui_menu/build.sh [out-binary]
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-/tmp/mp_ui_menu_test}
M=$root/decomp/src/Game/MP
OS_FLAGS="-fshort-wchar"
[ "$(uname -s)" = "Darwin" ] && OS_FLAGS="-Wl,-undefined,dynamic_lookup"
nice -n 10 ${CXX:-g++} -std=c++20 $OS_FLAGS -fsanitize=address,undefined -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -I"$root/decomp/include" -I"$root/platform/include" -I"$root/tests/mp" -I"$root/platform/src/mp" -I"$root/tests" \
  "$root"/tests/mp/ui_menu/*.cpp $M/MpMenu*.cpp $M/MpToast.cpp $M/MpMessages.cpp $M/MpMessagesExtra.cpp $M/MpPanelPlan.cpp $M/MpEntryLogic.cpp \
  "$root/platform/src/settings/settings_native.cpp" "$root/platform/src/settings/player_settings.cpp" "$root/platform/src/settings/settings_store.cpp" \
  "$root/platform/src/input/control_bindings.cpp" "$root/tests/mp/mp_gather_fake.cpp" "$root/platform/src/mp/mp_direct_code.cpp" "$root/platform/src/mp/mp_invite.cpp" \
  -o "$out"
cd "$root" && nice -n 10 "$out" "${@:2}"

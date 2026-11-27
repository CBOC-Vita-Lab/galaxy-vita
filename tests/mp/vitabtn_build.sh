#!/bin/bash
# Build and run the Vita-controls host test (tests/mp/vitabtn_test.cpp): no Wii control word or picture in any multiplayer text of the five
# languages, every action of MpControls.hpp has a Vita input, rows / heads / the guide band answer a touch. Run from anywhere.
#   tests/mp/vitabtn_build.sh
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_vitabtn_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
nice -n 10 $CXX -std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
  -I"$root/decomp/include" -I"$root/platform/include" -I"$root/tests/mp/include" -o "$out/vitabtn_test" "$root/tests/mp/vitabtn_test.cpp" \
  "$root/decomp/src/Game/MP/MpMessages.cpp" "$root/decomp/src/Game/MP/MpMessagesExtra.cpp" "$root/decomp/src/Game/MP/MpPanelPlan.cpp"
VB_ROOT="$root" "$out/vitabtn_test" "$@"

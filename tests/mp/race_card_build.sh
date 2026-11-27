#!/bin/sh
# Build and run the race results-card / head start line host test (MpToast.cpp, MpPanelPlan.cpp, MpMessages*.cpp). Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
D=decomp/src/Game/MP
g++ -std=c++20 -fshort-wchar -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -Wno-unused-parameter -Idecomp/include -Iplatform/include \
  tests/mp/race_card_test.cpp $D/MpToast.cpp $D/MpPanelPlan.cpp $D/MpMessages.cpp $D/MpMessagesExtra.cpp -o $MP_TMP/race_card_test
$MP_TMP/race_card_test

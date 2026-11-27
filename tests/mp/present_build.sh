#!/bin/sh
# Build and run the in-play presentation host tests (tests/mp/present_test.cpp) under ASan + UBSan. Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
SRCS="decomp/src/Game/MP/MpMessages.cpp decomp/src/Game/MP/MpMessagesExtra.cpp platform/src/mp/mp_config.cpp platform/src/mp/mp_core_sys.cpp
 platform/src/mp/mp_log.cpp"
nice -n 10 $CXX -std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Wno-unused-parameter \
  -Wno-missing-field-initializers -I decomp/include -I platform/include -I platform/src/mp tests/mp/present_test.cpp $SRCS -lpthread -o $MP_TMP/present_test
$MP_TMP/present_test

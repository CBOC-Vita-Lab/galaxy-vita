#!/bin/bash
# flow (keepsafe F1-F4): flow.cfg / coop.cfg across game versions (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_flow_store}
mkdir -p "$out"
F="-DTARGET_PC=1 -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -I$root/platform/include -I$root/platform/src/mp"
nice -n 10 g++ $F -o "$out/flow_store_test" "$root/tests/mp/flow_store_test.cpp" "$root/platform/src/mp/mp_ui_flow.cpp" \
  "$root/platform/src/mp/mp_savefile.cpp" "$root/platform/src/mp/mp_savecheck.cpp" -lpthread
nice -n 10 "$out/flow_store_test"

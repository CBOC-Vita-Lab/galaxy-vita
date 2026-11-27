#!/bin/bash
# optfix item 3: the composition key of the in-play panels (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_compose_key}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
  -I"$root/decomp/include" -I"$root/platform/include" -I"$root/tests/mp" \
  -o "$out/compose_key_test" "$root/tests/mp/compose_key_test.cpp" "$root/decomp/src/Game/MP/MpPanelPlan.cpp"
nice -n 10 "$out/compose_key_test"

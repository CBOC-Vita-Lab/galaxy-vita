#!/bin/bash
# quota32: the busy / out-of-quota classifier and its words (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_svc_busy}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra \
  -I"$root/platform/include" -I"$root/platform/src/mp" -I"$root/decomp/include" \
  -o "$out/svc_busy_test" "$root/tests/mp/svc_busy_test.cpp" "$root/decomp/src/Game/MP/MpMessages.cpp" "$root/decomp/src/Game/MP/MpMessagesExtra.cpp"
nice -n 10 "$out/svc_busy_test"

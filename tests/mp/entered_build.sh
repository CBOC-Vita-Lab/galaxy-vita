#!/bin/bash
# optfix: the entered-flag retirement rules (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_entered}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/include" -I"$root/decomp/include" \
  -o "$out/entered_test" "$root/tests/mp/entered_test.cpp"
nice -n 10 "$out/entered_test"

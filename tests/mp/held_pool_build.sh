#!/bin/bash
# optfix item 15: the held-message byte pool (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_held_pool}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/include" -I"$root/decomp/include" \
  -o "$out/held_pool_test" "$root/tests/mp/held_pool_test.cpp"
nice -n 10 "$out/held_pool_test"

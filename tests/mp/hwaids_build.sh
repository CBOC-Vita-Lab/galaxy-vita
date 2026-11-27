#!/bin/bash
# hwaids: host test of the UDP log mirror's pure logic (ASan + UBSan). Quick: run directly at nice 10.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_hwaids}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/src/mp" \
  -o "$out/hwaids_test" "$root/tests/mp/hwaids_test.cpp"
nice -n 10 "$out/hwaids_test"

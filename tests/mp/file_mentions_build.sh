#!/bin/bash
# optfix item 10: the chunked file search of the Wi-Fi Turbo detection (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_file_mentions}
mkdir -p "$out"
F="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -I$root/platform/include -I$root/platform/src/mp"
nice -n 10 g++ $F -o "$out/file_mentions_test" "$root/tests/mp/file_mentions_test.cpp" "$root/platform/src/mp/mp_core_vita.cpp" -lpthread
nice -n 10 "$out/file_mentions_test"

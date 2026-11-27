#!/bin/bash
# core2: the spoken code (mp_direct_code) and the gathering fake for the menu tests (ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_gather_tests}
mkdir -p "$out"
F="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I$root/platform/include -I$root/tests/mp"
g++ $F "$root/tests/mp/direct_code_test.cpp" "$root/platform/src/mp/mp_direct_code.cpp" -o "$out/direct_code_test"
g++ $F "$root/tests/mp/mp_gather_fake_test.cpp" "$root/tests/mp/mp_gather_fake.cpp" "$root/platform/src/mp/mp_direct_code.cpp" "$root/platform/src/mp/mp_invite.cpp" -o "$out/mp_gather_fake_test"
"$out/direct_code_test"
"$out/mp_gather_fake_test"

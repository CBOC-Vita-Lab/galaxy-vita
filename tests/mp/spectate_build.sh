#!/bin/bash
# Build and run the spectate host tests (ASan + UBSan): the selection rules and the follow camera's maths.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_spectate_tests}
mkdir -p "$out"
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
nice -n 10 $CXX -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/decomp/include" \
  -o "$out/spectate_test" "$root/tests/mp/spectate_test.cpp"
"$out/spectate_test"

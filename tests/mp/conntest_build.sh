#!/bin/bash
# MP3 conntest: the Connection Test engine (platform/src/mp/mp_conntest.cpp): the verdict table and its invariants over every combination of
# results, the run through fake probes with a fake clock (budget, order, cancel, hostile probe answers, rooms), the helpers.
#   tests/mp/conntest_build.sh            build, run   (ASan + UBSan)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_conntest_tests}
mkdir -p "$out"
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
nice -n 10 $CXX -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
  -I"$root/platform/include" -I"$root/platform/src/mp" -o "$out/conntest_test" "$root/tests/mp/conntest_test.cpp" "$root/platform/src/mp/mp_conntest.cpp"
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/conntest_test"

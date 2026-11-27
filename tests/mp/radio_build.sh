#!/bin/bash
# Build and run the Vita ad hoc radio host tests: net/src/vlink_vita_radio.cpp against tests/mp/radio_stub (ASan + UBSan, g++ when clang has no runtime).
#   tests/mp/radio_build.sh [build]        no argument: build and run all; "build": build only;  RADIO_TEST_VERBOSE=1 shows station logs and stderr
#   run one test: $MP_TMP/mp_radio_tests/radio_test <substring>
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_radio_tests}; mkdir -p "$out"
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -Wno-format-zero-length -Wno-unused-result -DNP_TARGET_VITA -I$root/tests/mp/radio_stub/include -I$root/tests/mp/radio_stub -I$root/net/src -I$root/net/include"
nice -n 10 $CXX $FLAGS -c "$root/net/src/vlink_vita_radio.cpp" -o "$out/radio.o"
nice -n 10 $CXX $FLAGS -c "$root/tests/mp/radio_stub/radio_stub.cpp" -o "$out/radio_stub.o"
nice -n 10 $CXX $FLAGS "$root/tests/mp/radio_test.cpp" "$out/radio.o" "$out/radio_stub.o" -o "$out/radio_test" -lpthread
echo "built in $out"
[ "$1" = build ] || nice -n 10 "$out/radio_test"

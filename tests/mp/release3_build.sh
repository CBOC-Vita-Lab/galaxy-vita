#!/bin/bash
# Galaxy 3.0 multiplayer (release3): host test of the update notice (mp_update) and the report scrubber / builder (mp_report), ASan + UBSan.
#   tests/mp/release3_build.sh          build and run release3_test
# (g++: this machine's clang has no sanitizer runtime). No game, no network. Quick (< 1 min).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_release3_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/platform/src/mp"
nice -n 10 $CXX $FLAGS -o "$out/release3_test" "$root/tests/mp/release3_test.cpp" "$root/platform/src/mp/mp_update.cpp" "$root/platform/src/mp/mp_report.cpp" "$root/platform/src/mp/mp_config.cpp" "$root/platform/src/mp/mp_core_sys.cpp" "$root/platform/src/mp/mp_log.cpp" -lpthread
"$out/release3_test"

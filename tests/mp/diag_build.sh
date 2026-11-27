#!/bin/bash
# Galaxy 3.0 multiplayer (beta diagnostics): host test of the problem report's body (masking, cap, JSON), ASan + UBSan.
#   tests/mp/diag_build.sh          build and run diag_test (no game, no network; quick)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_diag_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/platform/src/mp"
nice -n 10 $CXX $FLAGS -o "$out/diag_test" "$root/tests/mp/diag_test.cpp" "$root/platform/src/mp/mp_diag_text.cpp" "$root/platform/src/mp/mp_report.cpp" "$root/platform/src/mp/mp_config.cpp" "$root/platform/src/mp/mp_core_sys.cpp" "$root/platform/src/mp/mp_log.cpp" -lpthread
"$out/diag_test"

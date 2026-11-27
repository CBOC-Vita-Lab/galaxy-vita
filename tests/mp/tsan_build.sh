#!/bin/bash
# Build the threaded host tests with ThreadSanitizer (g++; this machine's clang has no sanitizer runtimes).
#   tests/mp/tsan_build.sh            build mp_bot, perf_core_test, soak_long, save_test, service tests into $MP_TEST_OUT
# Run: see tests/mp/tsan_run.sh
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_tsan}
mkdir -p "$out"
if [ -f "$root/net/src/vlink.cpp" ]; then vl="$root/net/src"; vlinc="-I$root/net/include -I$root/net/include/online";
else ref=/home/claude/g3/ref/strikers/native; vl="$ref/platform/sdk/net"; vlinc="-I$root/tests/mp/include -I$ref"; fi
CXX=${CXX:-g++}
FLAGS="-std=c++20 -g -O1 -fsanitize=thread -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/platform/src/mp -I$root/decomp/include $vlinc"
SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
 $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
 $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $root/tests/mp/netglue_stub.cpp $vl/vlink.cpp $vl/vlink_udp.cpp"
build() { local o=$1; shift; rm -f "$out/$o"; nice -n 10 $CXX $FLAGS -o "$out/$o" "$@" -lpthread; }   # (foreground: a failed compile stops the script and leaves no stale binary; it was `&` + a bare wait, which hid the failure)

build perf_core_test "$root/tests/mp/perf_core_test.cpp" $SRCS
build soak_long "$root/tests/mp/soak_long.cpp" $SRCS
build core_tests "$root/tests/mp/core_tests.cpp" $SRCS
build mp_bot "$root/tools/mp/mp_bot.cpp" $SRCS
build save_test "$root/tests/mp/save_test.cpp" "$root/platform/src/mp/mp_savecheck.cpp" "$root/platform/src/mp/mp_savefile.cpp"
echo "built in $out"
#!/bin/bash
# Build and run the perf agent's host tests (ASan + UBSan): the session layer's game-thread paths, the puppet clock and (perf3)
# the quality governor.
#   tests/mp/perf_build.sh            build all, run
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_perf_tests}
mkdir -p "$out"
if [ -f "$root/net/vlink.cpp" ]; then vl="$root/net"; vlinc="-I$root/net -I$root/net/include";
else ref=/home/claude/g3/ref/strikers/native; vl="$ref/platform/sdk/net"; vlinc="-I$root/tests/mp/include -I$ref"; fi
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include $vlinc"
SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
 $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
 $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $vl/vlink.cpp $vl/vlink_udp.cpp"
# (integ: the core calls mp_netglue_frame; host tests link the stub)
EXTRA=""; [ -f "$root/tests/mp/netglue_stub.cpp" ] && EXTRA="$root/tests/mp/netglue_stub.cpp"
nice -n 10 $CXX $FLAGS -o "$out/perf_core_test" "$root/tests/mp/perf_core_test.cpp" $SRCS $EXTRA -lpthread
[ -f "$root/tests/mp/perf_pose_test.cpp" ] && nice -n 10 $CXX $FLAGS -I$root/decomp/include -o "$out/perf_pose_test" "$root/tests/mp/perf_pose_test.cpp"
# perf3: the quality governor (decomp/include/Game/MP/MpQualityLogic.hpp)
nice -n 10 $CXX $FLAGS -I$root/decomp/include -o "$out/quality_test" "$root/tests/mp/quality_test.cpp"
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/perf_core_test"
[ -x "$out/perf_pose_test" ] && "$out/perf_pose_test"
"$out/quality_test"
# MP3 nocap: the layout without the fourth core (and the static check that no session thread asks for it)
bash "$root/tests/mp/nocap_build.sh"

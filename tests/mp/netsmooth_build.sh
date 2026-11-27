#!/bin/bash
# Build and run the netsmooth agent's host tests (ASan + UBSan) and the smoothing simulator (-O2).
#   tests/mp/netsmooth_build.sh            build, run the tests, then the simulator on its synthetic course (2 seeds per case)
#   tests/mp/netsmooth_build.sh build      build only
#   SIM_TRACE=<trace> tests/mp/netsmooth_build.sh   the simulator on a recorded trace (tools/mp/netsmooth/poselog.py trace)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_netsmooth_tests}
mkdir -p "$out"
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o $MP_TMP/.mp_asan_probe 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
INC="-I$root/platform/include -I$root/decomp/include -I$root/tests/mp"
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function"
nice -n 10 $CXX $FLAGS $INC -o "$out/netsmooth_test" "$root/tests/mp/netsmooth_test.cpp"
nice -n 10 $CXX $FLAGS $INC -o "$out/puppet_pose_test" "$root/tests/mp/puppet_pose_test.cpp"
nice -n 10 $CXX $FLAGS $INC -o "$out/perf_pose_test" "$root/tests/mp/perf_pose_test.cpp"
nice -n 10 $CXX -std=c++20 -O2 $INC -o "$out/netsmooth_sim" "$root/tests/mp/netsmooth_sim.cpp"
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/netsmooth_test"
"$out/puppet_pose_test" | tail -1
"$out/perf_pose_test" | tail -1
nice -n 10 "$out/netsmooth_sim" --seeds 2 ${SIM_TRACE:+--trace "$SIM_TRACE"}

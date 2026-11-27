#!/bin/bash
# Build and run the session-layer host tests (ASan + UBSan).
#   tests/mp/build.sh            build all, run core_tests and core_fuzz
#   tests/mp/build.sh build      build only
# Env knobs for core_tests: MP_ONLY=<substring> runs only the tests whose name contains it (e.g. MP_ONLY=Soak, MP_ONLY=Netpack);
#   MP_SEED=n shifts every random seed (same tests, other timings); MP_BACKOFF_SHIFT=n sets the resend backoff ceiling (2 = 4x);
#   MP_DUPDEBUG=1 prints the first duplicate game events of the soak.
# soak_exact (netcore): the exactly-once soak, standalone (-seed n -creep n -shift n -v): one SOAK line, exit 1 on a duplicate, a hole,
#   a lost tail or no convergence. tests/mp/soak_matrix.sh <bin> <seeds> "<shifts>" "<creeps>" runs a matrix of it.
# VLink for the mp_bot / VLink transport build comes from the vendored net/ when present, else from the Strikers reference.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_core_tests_$(basename "$root")}   # (per tree: agents sharing one default folder ran each other's stale binaries)
mkdir -p "$out"
if [ -f "$root/net/src/vlink.cpp" ]; then vl="$root/net/src"; VLINC=("-I$root/net/include");
else ref=/home/claude/g3/ref/strikers/native; vl="$ref/platform/sdk/net"; VLINC=("-I$root/tests/mp/include" "-I$ref"); fi
# (clang++ is preferred, but this machine's clang has no sanitizer runtime: g++ is used when clang cannot link -fsanitize)
if [ -z "$CXX" ]; then
  if echo 'int main(){}' | clang++ -fsanitize=address -x c++ - -o "$MP_TMP/.mp_asan_probe" 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
FLAGS=(-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function)
[ "$(uname -s)" = "Darwin" ] && FLAGS+=(-Wl,-undefined,dynamic_lookup)
FLAGS+=("-I$root/platform/include" "${VLINC[@]}")
SRCS=(
  "$root/platform/src/mp/mp_core.cpp" "$root/platform/src/mp/mp_demo.cpp" "$root/platform/src/mp/mp_core_sys.cpp"
  "$root/platform/src/mp/mp_core_vita.cpp" "$root/platform/src/mp/mp_reliable.cpp" "$root/platform/src/mp/mp_lan.cpp"
  "$root/platform/src/mp/mp_snapshot.cpp" "$root/platform/src/mp/mp_config.cpp" "$root/platform/src/mp/mp_log.cpp"
  "$root/platform/src/mp/mp_transport_loop.cpp" "$root/platform/src/mp/mp_transport_vlink.cpp" "$root/platform/src/mp/mp_conn_online.cpp"
  "$root/tests/mp/netglue_stub.cpp" "$vl/vlink.cpp" "$vl/vlink_udp.cpp"
)
nice -n 10 "$CXX" "${FLAGS[@]}" -o "$out/core_tests" "$root/tests/mp/core_tests.cpp" "${SRCS[@]}" -lpthread
nice -n 10 "$CXX" "${FLAGS[@]}" -o "$out/core_fuzz" "$root/tests/mp/core_fuzz.cpp" "${SRCS[@]}" -lpthread
nice -n 10 "$CXX" "${FLAGS[@]}" -o "$out/reconnect_probe_test" "$root/tests/mp/reconnect_probe_test.cpp" "${SRCS[@]}" -lpthread
nice -n 10 "$CXX" "${FLAGS[@]}" -o "$out/mp_bot" "$root/tools/mp/mp_bot.cpp" "${SRCS[@]}" -lpthread
nice -n 10 "$CXX" "${FLAGS[@]}" -DSOAK_HAVE_CREEP -DSOAK_HAVE_SHIFT -o "$out/soak_exact" "$root/tests/mp/soak_exact.cpp" "${SRCS[@]}" -lpthread
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/core_tests"
"$out/core_fuzz"
"$out/reconnect_probe_test"
for seed in 0 1 2 3 4 5 6 7; do "$out/soak_exact" -seed $seed || exit 1; done
nice -n 10 "$CXX" "${FLAGS[@]}" -I"$root/net/src" -I"$root/net/src/online" -o "$out/friendcode_test" "$root/tests/mp/friendcode_test.cpp" && "$out/friendcode_test"

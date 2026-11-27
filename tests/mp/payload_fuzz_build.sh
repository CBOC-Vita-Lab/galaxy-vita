#!/bin/bash
# Build and run the structure-aware message fuzz (tests/mp/payload_fuzz.cpp) under ASan + UBSan.
#   tests/mp/payload_fuzz_build.sh [iterA=40000] [iterB=300000] [iterC=300000] [seed=1]     (MP_FUZZ_BUILD_ONLY=1: build only)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_core_tests}
mkdir -p "$out"
ref=/home/claude/g3/ref/strikers/native
if [ -f "$root/net/src/vlink.cpp" ]; then
  vl="$root/net/src"
  vlinc=("-I$root/net/include")
else
  vl="$ref/platform/sdk/net"
  vlinc=("-I$root/tests/mp/include" "-I$ref")
fi
CXX=${CXX:-g++}
WL_WEAK=()
[ "$(uname -s)" = "Darwin" ] && WL_WEAK=("-Wl,-undefined,dynamic_lookup")
FLAGS=(-std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function "${WL_WEAK[@]}" "-I$root/platform/include" "-I$root/decomp/include" "${vlinc[@]}")
SRCS=("$root/platform/src/mp/mp_core.cpp" "$root/platform/src/mp/mp_demo.cpp" "$root/platform/src/mp/mp_core_sys.cpp" "$root/platform/src/mp/mp_core_vita.cpp" "$root/platform/src/mp/mp_reliable.cpp" "$root/platform/src/mp/mp_lan.cpp"
 "$root/platform/src/mp/mp_snapshot.cpp" "$root/platform/src/mp/mp_config.cpp" "$root/platform/src/mp/mp_log.cpp" "$root/platform/src/mp/mp_transport_loop.cpp"
 "$root/platform/src/mp/mp_transport_vlink.cpp" "$root/platform/src/mp/mp_conn_online.cpp" "$root/tests/mp/netglue_stub.cpp" "$vl/vlink.cpp" "$vl/vlink_udp.cpp"
 "$root/decomp/src/Game/MP/MpWorldLogic.cpp")
nice -n 10 "$CXX" "${FLAGS[@]}" -o "$out/payload_fuzz" "$root/tests/mp/payload_fuzz.cpp" "${SRCS[@]}" -lpthread
[ -n "$MP_FUZZ_BUILD_ONLY" ] && exit 0
ASAN_OPTIONS=quarantine_size_mb=1 nice -n 10 "$out/payload_fuzz" "$@"

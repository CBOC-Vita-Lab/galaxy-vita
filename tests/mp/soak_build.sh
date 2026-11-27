#!/bin/bash
# Build and run the long chaos soak (tests/mp/soak_long.cpp) under ASan + UBSan.
#   tests/mp/soak_build.sh [minutes=15] [seed=1]      (MP_SOAK_BUILD_ONLY=1: build only)
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_core_tests}
mkdir -p "$out"
ref=/home/claude/g3/ref/strikers/native
if [ -f "$root/net/src/vlink.cpp" ]; then vl="$root/net/src"; vlinc="-I$root/net/include"; else vl="$ref/platform/sdk/net"; vlinc="-I$root/tests/mp/include -I$ref"; fi
CXX=${CXX:-g++}
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/decomp/include $vlinc"
SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
 $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
 $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $root/tests/mp/netglue_stub.cpp $vl/vlink.cpp $vl/vlink_udp.cpp"
nice -n 10 $CXX $FLAGS -o "$out/soak_long" "$root/tests/mp/soak_long.cpp" $SRCS -lpthread
[ -n "$MP_SOAK_BUILD_ONLY" ] && exit 0
ASAN_OPTIONS=quarantine_size_mb=1 nice -n 10 "$out/soak_long" "$@"

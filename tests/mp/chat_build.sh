#!/bin/bash
# Build and run the chat host tests (tests/mp/chat_test.cpp) under ASan + UBSan: the CHAT codec, text cleaning, the rate limit,
# the ring / mute / balloon logic and the session layer's host checks (three loopback consoles).
#   tests/mp/chat_build.sh [-v]
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_chat_tests}
mkdir -p "$out"
if [ -f "$root/net/src/vlink.cpp" ]; then vl="$root/net/src"; vlinc="-I$root/net/include";
else ref=/home/claude/g3/ref/strikers/native; vl="$ref/platform/sdk/net"; vlinc="-I$root/tests/mp/include -I$ref"; fi
CXX=${CXX:-g++}
FLAGS="-std=c++20 -fshort-wchar -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/decomp/include $vlinc"
SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
 $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
 $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $root/tests/mp/netglue_stub.cpp $vl/vlink.cpp $vl/vlink_udp.cpp"
nice -n 10 $CXX $FLAGS -o "$out/chat_test" "$root/tests/mp/chat_test.cpp" $SRCS -lpthread
"$out/chat_test" "$@"

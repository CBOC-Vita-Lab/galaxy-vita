#!/bin/bash
# netfix: gathering tests that need the real core (ASan + UBSan): gather_soak (quick settings) and gather_migrate_test.
#   tests/mp/gather_core_build.sh [build]
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_gather_core_tests}
mkdir -p "$out"
if [ -f "$root/net/src/vlink.cpp" ]; then vl="$root/net/src"; vlinc="-I$root/net/include"; else ref=/home/claude/g3/ref/strikers/native; vl="$ref/platform/sdk/net"; vlinc="-I$root/tests/mp/include -I$ref"; fi
CXX=${CXX:-g++}
FLAGS="$XFLAGS -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include $vlinc"
SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
 $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
 $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $root/platform/src/mp/mp_gather.cpp $root/platform/src/mp/mp_direct_code.cpp $root/platform/src/mp/mp_invite.cpp $root/tests/mp/netglue_stub.cpp $vl/vlink.cpp $vl/vlink_udp.cpp"
nice -n 10 $CXX $FLAGS -o "$out/gather_migrate_test" "$root/tests/mp/gather_migrate_test.cpp" $SRCS -lpthread
nice -n 10 $CXX $FLAGS -o "$out/gather_code_test" "$root/tests/mp/gather_code_test.cpp" $SRCS -lpthread   # wire: the radio questions, the spoken code
nice -n 10 $CXX $FLAGS -o "$out/gather_invite_test" "$root/tests/mp/gather_invite_test.cpp" $SRCS -lpthread   # code5: the 5-character code
nice -n 10 $CXX $FLAGS -o "$out/gather_soak" "$root/tests/mp/gather_soak.cpp" $SRCS -lpthread
nice -n 10 $CXX $FLAGS -o "$out/gather_wifi_test" "$root/tests/mp/gather_wifi_test.cpp" $SRCS -lpthread   # mpwifi44: the Same Wi-Fi link that cannot be opened
nice -n 10 $CXX $FLAGS -o "$out/vlink_bind_ladder_test" "$root/tests/mp/vlink_bind_ladder_test.cpp"   # mp73: the Same Wi-Fi bind ladder
nice -n 10 $CXX $FLAGS -o "$out/vlink_scan_test" "$root/tests/mp/vlink_scan_test.cpp"   # mp74: finding a console that could only bind port 0
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/gather_migrate_test"
"$out/gather_code_test"
"$out/gather_invite_test"
"$out/gather_wifi_test"
"$out/vlink_bind_ladder_test"
"$out/vlink_scan_test"
"$out/gather_soak" -seeds 3

#!/bin/bash
# Galaxy 3.0 multiplayer (agent netx): host tests of internet play by address and of UPnP port mapping, under ASan/UBSan (g++: this
# machine's clang has no sanitizer runtime).
#   tests/mp/upnp_build.sh          build and run upnp_test (fake IGD on 127.0.0.1) and direct_test (two cores, join by address)
#   tests/mp/upnp_build.sh build    build only
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_netx_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
NETF="-std=gnu++17 -g -O1 $SAN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -fno-strict-aliasing -fwrapv -fsigned-char"
N=$root/net/src
NETSRC="$N/online/np_upnp.cpp $N/online/np_natpmp.cpp $N/online/np_net.cpp $N/online/np_udp_posix.cpp $N/online/np_stun.cpp $N/np_https_sys.cpp"
nice -n 10 ${CC:-gcc} -O1 -g -c -w $SAN $N/online/third_party/monocypher/monocypher.c -o "$out/monocypher.o"   # (np_natpmp: SystemRandom lives in np_crypto)
nice -n 10 $CXX $NETF -I$N -I$root/net/include -o "$out/upnp_test" "$root/tests/mp/upnp_test.cpp" $NETSRC $N/online/np_crypto.cpp "$out/monocypher.o" -lpthread
if [ -f "$root/tests/mp/direct_test.cpp" ]; then
  FLAGS="-std=c++20 -g -O1 $SAN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/net/include"
  SRCS="$root/platform/src/mp/mp_core.cpp $root/platform/src/mp/mp_demo.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp $root/platform/src/mp/mp_reliable.cpp $root/platform/src/mp/mp_lan.cpp
   $root/platform/src/mp/mp_snapshot.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_transport_loop.cpp
   $root/platform/src/mp/mp_transport_vlink.cpp $root/platform/src/mp/mp_conn_online.cpp $root/platform/src/mp/mp_conn_direct.cpp $root/platform/src/mp/mp_portmap.cpp
   $root/tests/mp/netglue_stub.cpp $N/vlink.cpp $N/vlink_udp.cpp $NETSRC $N/online/np_crypto.cpp $out/monocypher.o"
  nice -n 10 $CXX $FLAGS -o "$out/direct_test" "$root/tests/mp/direct_test.cpp" $SRCS -lpthread
fi
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/upnp_test"
[ -x "$out/direct_test" ] && "$out/direct_test"
exit 0

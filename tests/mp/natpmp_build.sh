#!/bin/bash
# Galaxy 3.0 multiplayer (agent natpmp): host test of the NAT-PMP / PCP client against a fake gateway on 127.0.0.1, under ASan/UBSan
# (g++: this machine's clang has no sanitizer runtime).
#   tests/mp/natpmp_build.sh          build and run natpmp_test
#   tests/mp/natpmp_build.sh build    build only
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_natpmp_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
NETF="-std=gnu++17 -g -O1 $SAN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -fno-strict-aliasing -fwrapv -fsigned-char"
N=$root/net/src
NETSRC="$N/online/np_natpmp.cpp $N/online/np_net.cpp $N/online/np_udp_posix.cpp $N/online/np_stun.cpp $N/online/np_crypto.cpp $N/np_https_sys.cpp"
nice -n 10 ${CC:-gcc} -O1 -g -c -w $SAN $N/online/third_party/monocypher/monocypher.c -o "$out/monocypher.o"
nice -n 10 $CXX $NETF -I$N -I$root/net/include -o "$out/natpmp_test" "$root/tests/mp/natpmp_test.cpp" $NETSRC "$out/monocypher.o" -lpthread
# the arbitration test (mp_portmap.cpp with both fake gateways)
nice -n 10 $CXX $NETF -I$N -I$root/net/include -I$root/platform/include -o "$out/portmap_test" "$root/tests/mp/portmap_test.cpp" "$root/platform/src/mp/mp_portmap.cpp" \
  "$root/platform/src/mp/mp_core_sys.cpp" "$root/platform/src/mp/mp_config.cpp" $N/online/np_upnp.cpp $NETSRC "$out/monocypher.o" -lpthread
echo "built in $out"
[ "$1" = "build" ] && exit 0
"$out/natpmp_test"
"$out/portmap_test"

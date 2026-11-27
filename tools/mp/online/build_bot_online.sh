#!/bin/bash
# Build mp_bot WITH the online stack (account, room codes, STUN / punch / TURN, HTTPS through Mbed TLS) for a Linux x86-64 box.
#   tools/mp/online/build_bot_online.sh <outdir> [BUILD_ID] [DEFAULT_SERVICE_URL]
#   env: LDEXTRA=-static (a binary that runs on any x86-64 Linux, e.g. a VPS); MBEDTLS=<host64 prefix> (default /tls/mbedtls/host64; make one with: python3 tools/mp/build-mbedtls.py --target host64 --work <dir>)
# BUILD_ID MUST equal the build id of the console's build (the fingerprint, "[mp] core data hash=.. fingerprint=.." in the Vita log;
# test builds use 3.1-dev, a release 3.0.0). The data hash comes at run time: --data-hash aa3ddccb (the disc's StageData file list).
# DEFAULT_SERVICE_URL defaults to the build's own (https://galaxy.psvita.dev); SMG_ONLINE_URL / --service overrides it at run time.
set -eo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:?outdir}; BID=${2:-3.1-dev}; URL=${3:-https://galaxy.psvita.dev}
MB=${MBEDTLS:-/tls/mbedtls/host64}
[ -f "$MB/lib/libmbedtls.a" ] || { echo "no Mbed TLS host64 prefix at $MB"; exit 2; }
mkdir -p "$out/obj"
N=$root/net/src; O=$N/online; M=$root/platform/src/mp
INC="-I$root/platform/include -I$root/net/include -I$root/net/src -I$root/net/src/online -I$M -I$MB/include"
NETFL="-DNP_NATIVE=1 -DNP_HAVE_MBEDTLS=1 -DNP_TARGET_HOST32=1 -DNDEBUG=1 -fno-strict-aliasing -fwrapv -fno-short-enums -fsigned-char -fno-exceptions -fno-rtti -O1 -g1 -w"
MPFL="-std=c++20 -O1 -g -w -DMP_BUILD_ID=\"$BID\" -DNP_ONLINE_URL=\"$URL\" -DMP_ALLOW_HTTP_URL=1"
# the net stack (the game's smg_net list, CMakeLists.txt, + the mbedtls transport)
NETSRC="$N/vlink.cpp $N/vlink_udp.cpp $N/vlink_online.cpp $N/np_glue.cpp $N/np_keyboard_host.cpp $N/np_https.cpp $N/np_https_sys.cpp $N/np_https_tls_mbedtls.cpp
 $O/np_net.cpp $O/np_crypto.cpp $O/np_hash.cpp $O/np_turn.cpp $O/np_stun.cpp $O/np_peer.cpp $O/np_online.cpp $O/np_fault.cpp $O/np_signal_fake.cpp
 $O/np_signal_http.cpp $O/np_http.cpp $O/np_http_plain.cpp $O/np_udp_posix.cpp $O/np_upnp.cpp $O/np_natpmp.cpp"
MPSRC="$M/mp_core.cpp $M/mp_demo.cpp $M/mp_core_sys.cpp $M/mp_core_vita.cpp $M/mp_reliable.cpp $M/mp_lan.cpp $M/mp_snapshot.cpp $M/mp_config.cpp $M/mp_log.cpp
 $M/mp_transport_loop.cpp $M/mp_transport_vlink.cpp $M/mp_conn_online.cpp $M/mp_conn_direct.cpp $M/mp_portmap.cpp $M/mp_netglue.cpp $M/mp_service.cpp
 $M/mp_invite.cpp $M/mp_gather.cpp $M/mp_direct_code.cpp $M/mp_keepsafe.cpp $M/mp_savefile.cpp $M/mp_savecheck.cpp $root/tests/mp/mp_bridge_weak_test.cpp $root/tools/mp/mp_bot.cpp"
objs=()
cc1() { local src=$1 o="$out/obj/$(basename "$1").o"; shift; objs+=("$o"); [ "$o" -nt "$src" ] && [ -z "$FORCE" ] || nice -n 10 "$@" -c "$src" -o "$o"; }
objs+=("$out/obj/monocypher.o"); nice -n 10 clang $INC -O1 -g1 -w -c $O/third_party/monocypher/monocypher.c -o "$out/obj/monocypher.o"
for f in $NETSRC; do cc1 $f clang++ -std=gnu++20 $INC $NETFL; done
for f in $MPSRC; do cc1 $f clang++ $INC $MPFL; done
nice -n 10 clang++ $LDEXTRA -o "$out/mp_bot" "${objs[@]}" "$MB/lib/libmbedtls.a" "$MB/lib/libmbedx509.a" "$MB/lib/libmbedcrypto.a" -lpthread
echo "built $out/mp_bot (build id $BID, service $URL)"

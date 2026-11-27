#!/bin/bash
# Builds and runs the tests of the online transport (net/src/online) on the host (x86-64, M=-m64 default here; Strikers' default was -m32):
# STUN, hole punching against every NAT type, the encrypted session, loss / delay / reordering, NAT timeouts, Wi-Fi drop and rebuild, the
# relay framing, and Galaxy's 4-console star rig (host migration). Virtual time: seconds.
#   net/tests/run_online_tests.sh [outdir] [-v]       default /tmp/np-online-test       SAN="-fsanitize=address,undefined" for the sanitizers (CXX=g++ CC=gcc where clang has no ASan runtime)
set -e
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/np-online-test}
mkdir -p "$OUT"
M=${M:--m64}
FL="$M -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNDEBUG=1 \
 -Isrc -Iinclude -Itests -fno-strict-aliasing -fwrapv -fno-short-enums -fsigned-char -std=gnu++11 -fno-exceptions -fno-rtti -O1 -g1 -Wall -Wno-unused-function $SAN"
N=src/online
${CC:-clang} $M -O1 -g1 -c -w $SAN $N/third_party/monocypher/monocypher.c -o "$OUT/monocypher.o"
${CXX:-clang++} $FL $N/np_net.cpp $N/np_crypto.cpp $N/np_hash.cpp $N/np_turn.cpp $N/np_stun.cpp $N/np_peer.cpp $N/np_online.cpp $N/np_fault.cpp $N/np_signal_fake.cpp $N/np_signal_http.cpp $N/np_http.cpp $N/np_http_plain.cpp src/np_https.cpp src/np_https_sys.cpp $N/np_udp_posix.cpp \
  tests/natsim.cpp tests/turnsim.cpp tests/online_test.cpp "$OUT/monocypher.o" -x c++ -o "$OUT/online_test" \
  -lpthread 2>"$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
# smallfix: the keyboard's UTF-16 to UTF-8 (src/np_ime_utf8.h, used by np_online_ime_vita.cpp): header-only, runs in a second
${CXX:-clang++} -std=gnu++11 -Wall -Isrc $SAN tests/ime_utf8_test.cpp -o "$OUT/ime_utf8_test" && "$OUT/ime_utf8_test"
"$OUT/online_test" "${@:2}"

#!/bin/bash
# Galaxy 3.0 multiplayer, agent `net`: host tests of the vendored online stack (net/). No game data.
#   tests/mp/net_tests.sh                 the online transport + the 4-console star rig in virtual time (clang, x86-64)
#   SAN=1 tests/mp/net_tests.sh           the same with ASan/UBSan (GCC: this machine's clang has no ASan runtime)
#   FAKE=<server/fake/server.mjs> tests/mp/net_tests.sh   + signaling against the service's fake (GAME=smg), incl. the star over HTTP
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
NET="$(cd "$(dirname "$0")/../../net" && pwd)"
OUT=${OUT:-$MP_TMP/smg-net-tests}
if [ -n "$SAN" ]; then export CXX=g++ CC=gcc SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"; fi
if [ -n "$FAKE" ]; then
  GAME=smg FAKE_SERVER="$FAKE" NODE=${NODE:-/opt/node22/bin/node} bash "$NET/tests/run_signal_http_test.sh" "$OUT"
else
  bash "$NET/tests/run_online_tests.sh" "$OUT"
fi

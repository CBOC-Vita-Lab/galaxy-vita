#!/bin/bash
# core2: the build wiring end to end, against the local fake service (never the real address). The fake starts first; its local
# address goes through the build's own path (cmake/MpBuildDefs.cmake, as CMakeLists.txt / vita/CMakeLists.txt / build.py use it) into
# -DNP_ONLINE_URL / -DMP_BUILD_ID; the client test (tests/mp/service_test.cpp, SMG_E2E) then runs with no SMG_ONLINE_URL at all:
# register, quick match lookup, friend lookup, friends both ways by friend code, leaderboard submit and boards, the account's friend
# code handed to the session, and `server =` in mp.cfg still overriding the address compiled in. Then the room part: signaling
# (room create, join by code, the 4-console star over HTTP) against the same kind of fake (net/tests/run_signal_http_test.sh).
#   tests/mp/service_e2e_test.sh        (from anywhere; needs node >= 22.5 and cmake; NO_ROOMS=1 skips the room part)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
OUT=${OUT:-$MP_TMP/mp_service_e2e}
mkdir -p "$OUT"
INC="-Inet/include -Inet/include/online"
SANFLAGS=${SANFLAGS:--fsanitize=address,undefined -fno-sanitize-recover=undefined}

start_fake() {   # $1 log, rest: env; prints the URL
  local log=$1; shift
  env GAME=smg SMG_BOARD_UNVERIFIED=show SMG_FLAG_NEW_STAR=0 "$@" "$NODE" server/fake/server.mjs --no-https --http-port 0 >"$log" 2>&1 &
  echo $! >"$log.pid"
  for _ in $(seq 1 100); do grep -q 'http://127.0.0.1:' "$log" && break; sleep 0.1; done
  grep -o 'http://127.0.0.1:[0-9]*' "$log" | sed -n 1p
}
LOG=$OUT/fake.log
CLOG=$OUT/fake-capped.log
URL=$(start_fake "$LOG")
CURL=$(start_fake "$CLOG" REGISTER_MAX_PER_DAY=1)
trap 'kill $(cat "$LOG.pid" "$CLOG.pid") 2>/dev/null || true' EXIT
[ -n "$URL" ] && [ -n "$CURL" ] || { cat "$LOG" "$CLOG"; echo "fake server did not start"; exit 1; }
case "$URL" in http://127.0.0.1:*) ;; *) echo "refusing a non-local address: $URL"; exit 1;; esac
echo "fake service at $URL"

# the build's own flags for that address (a refusal here is the build refusing it)
DEFS=$(cmake -DNP_ONLINE_URL="$URL" -DMP_BUILD_ID=e2e -DMP_ALLOW_HTTP_URL=ON -P tests/mp/build_defs_check.cmake 2>&1 | grep '^-D')
echo "build flags: $(echo $DEFS)"
mapfile -t DEFARR <<<"$DEFS"
nice -n 10 g++ -std=c++20 $SANFLAGS -g -O1 -Wall -Wextra -DSMG_E2E=1 "${DEFARR[@]}" \
  -Iplatform/include -Iplatform/src/mp $INC \
  platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp platform/src/mp/mp_config.cpp platform/src/mp/mp_log.cpp platform/src/mp/mp_core_sys.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  tests/mp/mp_bridge_weak_test.cpp tests/mp/service_https_stub.cpp tests/mp/service_test.cpp -lpthread -o "$OUT/service_e2e"
env -u SMG_ONLINE_URL SMG_ONLINE_URL_CAPPED="$CURL" "$OUT/service_e2e"

# the build refuses what it must refuse
for bad in "http://example.org" "https://a/b" "https://x\"y"; do
  if cmake -DNP_ONLINE_URL="$bad" -P tests/mp/build_defs_check.cmake >/dev/null 2>&1; then echo "build accepted NP_ONLINE_URL=$bad"; exit 1; fi
done
if cmake -DMP_BUILD_ID="a b" -P tests/mp/build_defs_check.cmake >/dev/null 2>&1; then echo "build accepted MP_BUILD_ID='a b'"; exit 1; fi
[ "$(cmake -P tests/mp/build_defs_check.cmake 2>&1 | grep '^-D')" = '-DMP_BUILD_ID="dev"
-DNP_ONLINE_URL="https://galaxy.psvita.dev"' ] || { echo "the default build is not 'service https://galaxy.psvita.dev, build id dev'"; exit 1; }
for none in none ""; do   # an explicit 'no service' stays possible (host tests); there is no workers.dev fallback anywhere
  [ "$(cmake -DNP_ONLINE_URL="$none" -P tests/mp/build_defs_check.cmake 2>&1 | grep '^-D')" = '-DMP_BUILD_ID="dev"' ] || { echo "NP_ONLINE_URL='$none' did not give 'no service'"; exit 1; }
done
echo "build wiring: defaults (https://galaxy.psvita.dev, dev), none, and refusals: ok"

if [ -z "${NO_ROOMS:-}" ]; then
  GAME=smg FAKE_SERVER="$PWD/server/fake/server.mjs" NODE="$NODE" nice -n 10 bash net/tests/run_signal_http_test.sh "$OUT/net"
fi
echo "service e2e: ok"

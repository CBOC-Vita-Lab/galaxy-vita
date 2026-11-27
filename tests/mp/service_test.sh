#!/bin/bash
# Host test of the online service client (platform/src/mp/mp_service.cpp) against the local fake service with GAME=smg.
#   tests/mp/service_test.sh            (from the repository root; needs node >= 22.5 and server/node_modules not required)
# NET_INC: where np_https.h and online/np_json.h are (default: the vendored net/include, else the Strikers reference copies).
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
REF=/home/claude/g3/ref/strikers/native/platform/sdk/net
if [ -n "${NET_INC:-}" ]; then INC="-I$NET_INC -I$NET_INC/online"
elif [ -f net/include/np_https.h ]; then INC="-Inet/include -Inet/include/online"
else INC="-I$REF -I$REF/online"; fi
OUT=${OUT:-$MP_TMP/mp_service_test}
# SANFLAGS=-fsanitize=thread (tests/mp/tsan_run.sh): the same tests under ThreadSanitizer (g++).
SANFLAGS=${SANFLAGS:--fsanitize=address,undefined -fno-sanitize-recover=undefined}
# clang++ with ASan/UBSan as BRIEF.md asks; this container's clang has no sanitizer runtimes, so fall back to g++ (same flags)
if [ -z "${CXX:-}" ]; then
  if [ -z "${SANFLAGS##*thread*}" ]; then CXX=g++; elif echo 'int main(){}' | clang++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then CXX=clang++; else CXX=g++; fi
fi
echo "compiler: $CXX"
$CXX -std=c++20 $SANFLAGS -g -O1 -Wall -Wextra -Wno-unused-function \
  -Iplatform/include -Iplatform/src/mp $INC \
  platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp tests/mp/mp_bridge_weak_test.cpp tests/mp/service_https_stub.cpp tests/mp/service_test.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  -lpthread -o "$OUT"

LOG=$(mktemp)
# (boards: this test checks the client's parsing of rows other players see; the finding-13 visibility rules are the server suite's)
GAME=smg SMG_BOARD_UNVERIFIED=show SMG_FLAG_NEW_STAR=0 "$NODE" server/fake/server.mjs --no-https --http-port 0 >"$LOG" 2>&1 &
FAKE=$!
trap 'kill $FAKE 2>/dev/null || true; rm -f "$LOG"' EXIT
for _ in $(seq 1 100); do grep -q 'http://127.0.0.1:' "$LOG" && break; sleep 0.1; done
URL=$(grep -o 'http://127.0.0.1:[0-9]*' "$LOG" | sed -n 1p)
[ -n "$URL" ] || { cat "$LOG"; echo "fake server did not start"; exit 1; }
echo "fake service (GAME=smg) at $URL"
# service review: a second fake whose day allows one registration (the daily-cap part of the test)
CLOG=$(mktemp)
GAME=smg REGISTER_MAX_PER_DAY=1 "$NODE" server/fake/server.mjs --no-https --http-port 0 >"$CLOG" 2>&1 &
CAPPED=$!
trap 'kill $FAKE $CAPPED 2>/dev/null || true; rm -f "$LOG" "$CLOG"' EXIT
for _ in $(seq 1 100); do grep -q 'http://127.0.0.1:' "$CLOG" && break; sleep 0.1; done
CURL=$(grep -o 'http://127.0.0.1:[0-9]*' "$CLOG" | sed -n 1p)
SMG_ONLINE_URL=$URL SMG_ONLINE_URL_CAPPED=$CURL "$OUT"

# the client against a broken / hostile service (HTTP errors, junk JSON, truncation, resets, silence)
OUT2=${OUT2:-$MP_TMP/mp_service_errors_test}
$CXX -std=c++20 $SANFLAGS -g -O1 -Wall -Wextra -Wno-unused-function \
  -Iplatform/include -Iplatform/src/mp $INC \
  platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp tests/mp/mp_bridge_weak_test.cpp tests/mp/service_https_stub.cpp tests/mp/service_errors_test.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  -lpthread -o "$OUT2"
HLOG=$(mktemp)
"$NODE" tests/mp/hostile_server.mjs >"$HLOG" 2>&1 &
HOST=$!
trap 'kill $FAKE $CAPPED $HOST 2>/dev/null || true; rm -f "$LOG" "$CLOG" "$HLOG"' EXIT
for _ in $(seq 1 100); do grep -q 'hostile http://' "$HLOG" && break; sleep 0.1; done
HURL=$(grep -o 'http://127.0.0.1:[0-9]*' "$HLOG" | sed -n 1p)
[ -n "$HURL" ] || { cat "$HLOG"; echo "hostile server did not start"; exit 1; }
SMG_HOSTILE_URL=$HURL SMG_ONLINE_TIMEOUT_MS=1500 "$OUT2"

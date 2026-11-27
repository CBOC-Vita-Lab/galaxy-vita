#!/bin/bash
# Room signaling against a failing service (tests/mp/signal_backoff_test.cpp): the local fake (GAME=smg) behind tests/mp/flaky_proxy.mjs.
# Local only. About 30 s of real time. Compiles (run through tools/buildrun.sh with the other suites, or directly at nice -n 10).
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
OUT=${OUT:-$MP_TMP/signal_backoff_test}
N=net/src/online
mkdir -p "$OUT.d"
gcc -O1 -c -w $N/third_party/monocypher/monocypher.c -o "$OUT.d/monocypher.o"
g++ -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -Inet/src -Inet/include -I$N \
  $N/np_net.cpp $N/np_crypto.cpp $N/np_hash.cpp $N/np_signal_http.cpp $N/np_http.cpp $N/np_http_plain.cpp net/src/np_https.cpp net/src/np_https_sys.cpp \
  tests/mp/signal_backoff_test.cpp "$OUT.d/monocypher.o" -lpthread -o "$OUT"
LOG=$(mktemp); PLOG=$(mktemp); REQ=$(mktemp)
REGISTER_POW_BITS=0 GAME=smg "$NODE" server/fake/server.mjs --no-https --http-port 0 >"$LOG" 2>&1 &
FAKE=$!
trap 'kill $FAKE ${PROXY_PID:-} 2>/dev/null || true; rm -f "$LOG" "$PLOG" "$REQ"' EXIT
for _ in $(seq 1 100); do grep -q 'http://127.0.0.1:' "$LOG" && break; sleep 0.1; done
URL=$(grep -o 'http://127.0.0.1:[0-9]*' "$LOG" | sed -n 1p)
"$NODE" tests/mp/flaky_proxy.mjs "$URL" "$REQ" >"$PLOG" 2>&1 &
PROXY_PID=$!
for _ in $(seq 1 100); do grep -q 'proxy http://' "$PLOG" && break; sleep 0.1; done
PROXY=$(grep -o 'http://127.0.0.1:[0-9]*' "$PLOG" | sed -n 1p)
TOKEN=$(curl -s -X POST "$URL/v1/register" -H 'X-Msc-App: 3.0.0' -H 'X-Msc-Game: smg' -H 'Content-Type: application/json' -d '{"name":"Backoff"}' | sed -e 's/.*"token":"\([^"]*\)".*/\1/')
TOKEN2=$(curl -s -X POST "$URL/v1/register" -H 'X-Msc-App: 3.0.0' -H 'X-Msc-Game: smg' -H 'Content-Type: application/json' -d '{"name":"Joiner"}' | sed -e 's/.*"token":"\([^"]*\)".*/\1/')
[ ${#TOKEN} = 43 ] && [ ${#TOKEN2} = 43 ] || { echo "registration failed"; cat "$LOG"; exit 3; }
PROXY=$PROXY PROXY_LOG=$REQ TOKEN=$TOKEN TOKEN2=$TOKEN2 FAKE_URL=$URL "$OUT"

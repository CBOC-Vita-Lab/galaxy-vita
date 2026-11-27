#!/bin/bash
# The online unit tests plus the signaling client against the LOCAL fake of the online service (server/fake/server.mjs: the Worker's own code over
# in-memory SQLite, plain HTTP on localhost). Starts the fake, registers two players, runs net/tests/online_test with SMG_TEST_SERVER set.
#   FAKE_SERVER=<path to server/fake/server.mjs> NODE=<node> net/tests/run_signal_http_test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
ROOT=$(git rev-parse --show-toplevel)/server
FAKE=${FAKE_SERVER:-$ROOT/fake/server.mjs}
NODE=${NODE:-node}
OUT=${1:-/tmp/np-signal-http-test}
mkdir -p "$OUT"
PORT=${FAKE_PORT:-$((18000 + $$ % 2000))}
[ -f "$FAKE" ] || { echo "no fake server at $FAKE (set FAKE_SERVER)"; exit 2; }
GAME=${GAME:-smg} REGISTER_POW_BITS=${REGISTER_POW_BITS:-0} "$NODE" "$FAKE" --http-port $PORT --https-port $((PORT + 1)) > "$OUT/fake.log" 2>&1 &
FAKE_PID=$!
trap 'kill $FAKE_PID 2>/dev/null' EXIT
for i in $(seq 1 60); do curl -s "http://127.0.0.1:$PORT/v1/info" >/dev/null 2>&1 && break; sleep 0.25; done
reg() { curl -s -X POST "http://127.0.0.1:$PORT/v1/register" -H 'X-Msc-App: 3.0.0' -H 'X-Msc-Game: smg' -H 'Content-Type: application/json' -d "{\"name\":\"$1\",\"region\":2}" | sed -e 's/.*"token":"\([^"]*\)".*/\1/'; }
T0=$(reg Alice1); T1=$(reg Bobby2); T2=$(reg Carol3); T3=$(reg Dave44)   # (Galaxy: four accounts for the star)
[ ${#T0} = 43 ] && [ ${#T1} = 43 ] || { echo "registration failed"; cat "$OUT/fake.log"; exit 3; }
export SMG_TEST_SERVER=http://127.0.0.1:$PORT SMG_TEST_TOKEN0=$T0 SMG_TEST_TOKEN1=$T1
[ ${#T2} = 43 ] && [ ${#T3} = 43 ] && export SMG_TEST_TOKEN2=$T2 SMG_TEST_TOKEN3=$T3
SAN=${SAN} bash tests/run_online_tests.sh "$OUT" "${@:2}"

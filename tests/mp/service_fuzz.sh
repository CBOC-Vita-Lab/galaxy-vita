#!/bin/bash
# Fuzz of the client's parsing of service replies (np_signal_http + mp_service) against a hostile local service
# (tests/mp/fuzz_service.mjs), under ASan/UBSan. Beside tests/mp/json_limits_build.sh. Local only. Compiles: run through tools/buildrun.sh
# for long runs (FUZZ_ROUNDS, FUZZ_SEEDS); the default (3 seeds x 60 rounds) takes about a minute.
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
OUT=${OUT:-$MP_TMP/mp_service_fuzz}
N=net/src/online
mkdir -p "$OUT.d"
gcc -O1 -c -w $N/third_party/monocypher/monocypher.c -o "$OUT.d/monocypher.o"
g++ -std=gnu++20 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 \
  -Iplatform/include -Iplatform/src/mp -Inet/include -Inet/src -I$N \
  platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp tests/mp/mp_bridge_weak_test.cpp tests/mp/service_https_stub.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  $N/np_net.cpp $N/np_crypto.cpp $N/np_hash.cpp $N/np_signal_http.cpp $N/np_http.cpp $N/np_http_plain.cpp \
  tests/mp/service_fuzz.cpp "$OUT.d/monocypher.o" -lpthread -o "$OUT"
for seed in ${FUZZ_SEEDS:-1 2 3}; do
  LOG=$(mktemp)
  "$NODE" tests/mp/fuzz_service.mjs "$seed" >"$LOG" 2>&1 &
  PID=$!
  for _ in $(seq 1 100); do grep -q 'fuzz http://' "$LOG" && break; sleep 0.1; done
  URL=$(grep -o 'http://127.0.0.1:[0-9]*' "$LOG" | sed -n 1p)
  echo "seed $seed"
  FUZZ_URL=$URL "$OUT" || { kill $PID; rm -f "$LOG"; exit 1; }
  kill $PID 2>/dev/null || true
  rm -f "$LOG"
done

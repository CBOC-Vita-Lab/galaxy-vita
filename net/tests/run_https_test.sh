#!/bin/bash
# Builds and runs net/tests/https_test.cpp (the HTTPS client against a local server with a private test CA, tests/https_server.py needs python3 "cryptography").
#   net/tests/run_https_test.sh [outdir]     MBEDTLS=<prefix> (default /tls/mbedtls/host64)   CXX=clang++
set -eo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-/tmp/np-https-test}
MBEDTLS=${MBEDTLS:-/tls/mbedtls/host64}
mkdir -p "$OUT"; rm -f "$OUT/ports.json"
${CXX:-clang++} -std=c++17 -g -O1 -Wall -Wno-unused-function -DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 -Isrc -Iinclude -I"$MBEDTLS/include" \
  tests/https_test.cpp src/np_https.cpp src/np_https_sys.cpp src/np_https_tls_mbedtls.cpp src/online/np_http.cpp src/online/np_http_plain.cpp \
  "$MBEDTLS/lib/libmbedtls.a" "$MBEDTLS/lib/libmbedx509.a" "$MBEDTLS/lib/libmbedcrypto.a" -lpthread -o "$OUT/https_test"
python3 tests/https_server.py "$OUT" >"$OUT/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [ -f "$OUT/ports.json" ] && break; sleep 0.2; done
[ -f "$OUT/ports.json" ] || { cat "$OUT/server.log"; echo "https server did not start"; exit 1; }
"$OUT/https_test" "$OUT"

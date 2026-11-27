#!/bin/bash
# corefix2: how a popped batch of messages is routed when a handler ends the session / moves the scene (ASan + UBSan). Quick: run directly.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-/tmp/mp_drain}
mkdir -p "$out"
nice -n 10 g++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/platform/include" -I"$root/decomp/include" \
  -o "$out/drain_test" "$root/tests/mp/drain_test.cpp"
nice -n 10 "$out/drain_test"

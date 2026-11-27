#!/usr/bin/env bash
# ab27: host test of the in-run A/B driver and the registry's live knobs (tests/ab27_test.cpp), under ASan + UBSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cxx=${CXX:-g++}
nice -n 10 "$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" -I"$root/platform/include/port" \
  "$root/tests/ab27_test.cpp" -o "$out/ab27_test"
UBSAN_OPTIONS=halt_on_error=1 "$out/ab27_test"

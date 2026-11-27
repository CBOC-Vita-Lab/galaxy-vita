#!/bin/bash
# Build and run the Same Wi-Fi room list host tests (lan_scan_test: trust tiers, eviction, rate limit, names; lan_flood_test: hostile
# beacons) under ASan + UBSan. Small: no session layer.   tests/mp/lan_build.sh
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_lan_tests}
mkdir -p "$out"
CXX=${CXX:-g++}
FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I$root/platform/include -I$root/platform/src/mp"
SRCS="$root/platform/src/mp/mp_lan.cpp $root/platform/src/mp/mp_config.cpp $root/platform/src/mp/mp_log.cpp $root/platform/src/mp/mp_core_sys.cpp $root/platform/src/mp/mp_core_vita.cpp"
for t in lan_scan_test lan_flood_test; do
  [ -f "$root/tests/mp/$t.cpp" ] || continue
  nice -n 10 $CXX $FLAGS -o "$out/$t" "$root/tests/mp/$t.cpp" $SRCS -lpthread
  "$out/$t" 2> >(grep -v "^\[mp\]" >&2)
done

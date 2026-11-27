#!/bin/bash
# netport31: the online path under the console's link rule on the host (tests/mp/online_nolink_test.cpp; ASan + UBSan). Quick: run directly.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_online_nolink}
mkdir -p "$out"
CXX=${CXX:-g++}
nice -n 10 $CXX -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -I"$root/platform/include" -I"$root/platform/src/mp" -I"$root/net/include" -I"$root/net/src" \
  -o "$out/online_nolink_test" "$root/tests/mp/online_nolink_test.cpp" "$root/platform/src/mp/mp_conn_online.cpp" "$root/platform/src/mp/mp_transport_vlink.cpp" \
  "$root/platform/src/mp/mp_config.cpp" "$root/platform/src/mp/mp_log.cpp" "$root/platform/src/mp/mp_core_sys.cpp" \
  "$root/net/src/vlink.cpp" "$root/net/src/vlink_udp.cpp" "$root/net/src/np_glue.cpp" -lpthread
nice -n 10 "$out/online_nolink_test"

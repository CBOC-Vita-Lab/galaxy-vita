#!/bin/bash
# Host test of the mp.cfg `server =` key and the per-server account files (platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp + mp_config.cpp). Run through tools/buildrun.sh.
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
if [ -f net/include/np_https.h ]; then INC="-Inet/include -Inet/include/online"; else INC="-I/home/claude/g3/ref/strikers/native/platform/sdk/net -I/home/claude/g3/ref/strikers/native/platform/sdk/net/online"; fi
SAN=${SANFLAGS:--fsanitize=address,undefined -fno-sanitize-recover=undefined}
g++ -std=c++20 $SAN -g -O1 -Wall -Wextra -DNP_ONLINE_URL='"https://default.example.org"' \
  -Iplatform/include -Iplatform/src/mp $INC \
  platform/src/mp/mp_service.cpp platform/src/mp/mp_invite.cpp platform/src/mp/mp_config.cpp platform/src/mp/mp_log.cpp platform/src/mp/mp_core_sys.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  tests/mp/mp_bridge_weak_test.cpp tests/mp/service_https_stub.cpp tests/mp/service_server_test.cpp -lpthread -o $MP_TMP/service_server_test
$MP_TMP/service_server_test

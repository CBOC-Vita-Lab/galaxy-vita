#!/bin/sh
# keepfix: ThreadSanitizer run of mp_config_load against the helper thread's save (tests/mp/config_lock_test.cpp). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
out=${OUT:-$MP_TMP/config_lock_test}
g++ -std=c++20 -fsanitize=thread -g -O1 -Iplatform/include -Iplatform/src/mp -Idecomp/include tests/mp/config_lock_test.cpp \
  platform/src/mp/mp_config.cpp platform/src/mp/mp_core_sys.cpp platform/src/mp/mp_core_vita.cpp -pthread -o "$out"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out"
# keepfix 3: keepsafe's "Bring back settings" against the real settings code with a helper thread saving all the time (50 rounds)
g++ -std=c++20 -fsanitize=thread -g -O1 -Iplatform/include -Iplatform/src/mp -Idecomp/include tests/mp/keepsafe_cfg_test.cpp \
  platform/src/mp/mp_keepsafe.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp \
  platform/src/mp/mp_config.cpp platform/src/mp/mp_core_sys.cpp platform/src/mp/mp_core_vita.cpp -pthread -o "$out.cfg"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out.cfg"

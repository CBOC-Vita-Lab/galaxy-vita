#!/bin/bash
# All emu self-tests (port leases, bad-network relay, Vita limits, memory line, proposed scenarios). No game binary. About 3 minutes; run at nice 10, one at a time.
set -uo pipefail
cd "$(dirname "$0")/../.."
rc=0
for t in tests/mp/harness_ports_test.py tests/mp/link_relay_test.py tests/mp/harness_mem_test.py tests/mp/emu_scenarios_test.py; do
  echo "== $t"; nice -n 10 python3 -W ignore "$t" 2>&1 | tail -4; [ "${PIPESTATUS[0]}" -eq 0 ] || rc=1   # (the pipe into tail hid the test's exit code)
done
echo "== tests/mp/vita_limits_test.sh"; bash tests/mp/vita_limits_test.sh 2>&1 | tail -2; [ "${PIPESTATUS[0]}" -eq 0 ] || rc=1
exit $rc

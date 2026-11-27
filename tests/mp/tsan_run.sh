#!/bin/bash
# Run the threaded host tests under ThreadSanitizer (build them first: tests/mp/tsan_build.sh).
#   tests/mp/tsan_run.sh [seconds=120]      each mp_bot --loopback scenario runs for `seconds` (TSan is ~10x slower: the bots' clock is real)
# Every TSan report goes to $MP_TEST_OUT/*.log; exit status 1 when any log holds a report or a test failed.
set -uo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${MP_TEST_OUT:-$MP_TMP/mp_tsan}
secs=${1:-120}
export TSAN_OPTIONS="halt_on_error=0 exit_code=66 history_size=5"
bad=0
run() { local name=$1; shift; local t0=$SECONDS; nice -n 10 "$@" > "$out/$name.log" 2>&1; local rc=$?
  local n; n=$(grep -c "WARNING: ThreadSanitizer" "$out/$name.log")
  echo "$name: rc=$rc tsan_reports=$n ($((SECONDS - t0)) s)"; [ "$rc" != 0 ] || [ "$n" != 0 ] && bad=1; }
run perf_core_test "$out/perf_core_test"
run save_test "$out/save_test"
run core_tests "$out/core_tests"
run bot_clean "$out/mp_bot" --loopback 4 --duration "$secs" --quiet --verify
run bot_impaired "$out/mp_bot" --loopback 4 --duration "$secs" --loss 0.1 --dup 0.05 --delay 40 --jitter 20 --verify --quiet
run bot_heavy "$out/mp_bot" --loopback 4 --duration "$secs" --loss 0.3 --dup 0.2 --delay 80 --jitter 60 --events 5 --verify --quiet
run bot_leave "$out/mp_bot" --loopback 4 --duration "$secs" --leave-after $((secs / 2)) --loss 0.05 --verify --quiet
run service_test env SANFLAGS=-fsanitize=thread bash "$root/tests/mp/service_test.sh"
exit $bad

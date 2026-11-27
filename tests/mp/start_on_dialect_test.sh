#!/bin/bash
# HARNESS_START_ON's regex is written in the dialect of every other scenario regex (Python: \d \w \S ...). run_pair.sh must match it the same way
# (final run 14 Nov: latejoin_after_kills' guest never started because `grep -E` reads \d as a plain "d"). Pure shell, no game.
set -u
H="$(cd "$(dirname "$0")/../../tools/mp/harness" && pwd)"
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
t=$(mktemp); trap 'rm -f "$t"' EXIT
echo '1794681554983 [mp] world test kill: killed=3 serial=1' > "$t"
# shellcheck disable=SC1091
source "$H/common.sh"
if ! declare -F harness_start_on_match >/dev/null; then bad "common.sh has no harness_start_on_match"; exit 1; fi
harness_start_on_match '\[mp\] world test kill: killed=3 serial=\d+' "$t" && ok "\\d+ matches a digit" || bad "\\d+ does not match the host's line"
harness_start_on_match '\[mp\] world test kill: killed=\d+ serial=\d+' "$t" && ok "two \\d+" || bad "two \\d+"
harness_start_on_match '\[mp\] world test kill: killed=\w+ serial=\S+' "$t" && ok "\\w \\S" || bad "\\w \\S"
harness_start_on_match '\[mp\] world test kill: killed=4' "$t" && bad "a wrong number matched" || ok "a wrong number does not match"
harness_start_on_match '\[mp\] world test kill: killed=3 serial=[a-z]+' "$t" && bad "letters matched digits" || ok "a wrong class does not match"
# a regex grep cannot compile is an error (exit 2 with a message), not "no match yet" (it would wait for the host to end)
( harness_start_on_match '\[mp\] (unclosed' "$t" ) >/dev/null 2>"$t.err"; rc=$?
[ $rc = 2 ] && /usr/bin/grep -q 'not valid PCRE' "$t.err" && ok "a bad regex fails loudly" || bad "a bad regex did not fail loudly (rc=$rc)"
rm -f "$t.err"
# joinfix: a console that has not started has no console.log yet: no match (status 1), never the "not valid PCRE" exit 2
( harness_start_on_match 'anything' "$t.absent" ) 2>/dev/null; [ $? = 1 ] && ok "a missing console.log is no match yet" || bad "a missing console.log is not 'no match yet'"
# run_pair.sh must use that function (no private grep -E for the mark)
/usr/bin/grep -q 'harness_start_on_match "\$so_rx"' "$H/run_pair.sh" && ok "run_pair.sh uses it" || bad "run_pair.sh does not call harness_start_on_match"
/usr/bin/grep -qE 'grep -q?E[^|]*so_rx' "$H/run_pair.sh" && bad "run_pair.sh still has grep -E on so_rx" || ok "no grep -E on so_rx"
[ $fail = 0 ] && echo "start_on_dialect_test: PASS" || echo "start_on_dialect_test: FAIL"
exit $fail

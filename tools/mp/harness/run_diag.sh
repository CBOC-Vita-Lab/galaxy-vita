#!/bin/bash
# Runs the beta diagnostics scenario (diag_report): starts diag_stub.mjs (the fake online service + /v1/report + /v1/turn/check + a STUN/TURN server) on
# 127.0.0.1:18797 and UDP 3478, seeds this console's online account and the "last session ended in an error" state, runs the scenario through the game-run
# queue, stops the stub, then checks what the stub received (diag_check.py). Local only (the seeder refuses any other address).
#   tools/mp/harness/run_diag.sh <game binary> [more run_scenario.py args]
set -u
BIN=${1:?game binary}; shift
HERE=$(cd "$(dirname "$0")" && pwd)
P=18797
D=/home/claude/g3/hruns/diag-beta
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
pkill -f "diag_stub.mjs --port $P" 2>/dev/null; sleep 1
rm -rf "$D"; mkdir -p "$D/data/mp" "$D/online" "$D/stub"      # (only this scenario's own scratch folder)
LOG="$D/stub-console.log"
(setsid nohup "$NODE" "$HERE/diag_stub.mjs" --port $P --out "$D/stub" --reports 429,201 --delay-ms 4000 --stun-alt-b >"$LOG" 2>&1 &)
for _ in $(seq 1 120); do grep -q "^ready" "$LOG" && break; sleep 0.5; done
grep -q "^ready" "$LOG" || { cat "$LOG"; echo "stub did not start"; pkill -f "diag_stub.mjs --port $P"; exit 1; }
trap 'pkill -f "diag_stub.mjs --port $P" 2>/dev/null' EXIT
python3 -I "$HERE/seed_online_account.py" "http://127.0.0.1:$P" "$D/online" Alex >/dev/null || { echo "seeding the account failed"; exit 1; }
# the state a session that ended with an error leaves (mp_diag.cpp): E <pending> <code>, S <last summary>
printf 'E 1 error:sync\nS relay 321 3 error:sync 58.5 17.2 420.0 66.0 140.0 5 6 7 8 9 2 1 3.0.0\n' > "$D/data/mp/diag_last.txt"
export HARNESS_BIN=$BIN
/home/claude/g3/tools/gamerun.sh python3 "$HERE/run_scenario.py" "$HERE/scenarios/diag_report.yaml" "$@"
rc=$?
echo "scenario diag_report exit $rc"
python3 -I "$HERE/diag_check.py" "$D/stub" || rc=1
exit $rc

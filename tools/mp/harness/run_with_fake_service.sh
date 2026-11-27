#!/bin/bash
# Runs a scenario that needs the local fake online service (qa_lan_friend_codes, qa_friend_from_played_with): starts the fake from THIS tree on
# 127.0.0.1:18797, recreates the scenario's own scratch folders, runs the scenario through the game-run queue, stops the fake.
#   tools/mp/harness/run_with_fake_service.sh <qa_lan_friend_codes|qa_friend_from_played_with> <game binary> [more run_scenario.py args]
# Both scenarios stay out of sweep_order.txt (they need this wrapper). Needs node >= 22 (NODE=...). Port 18797 is this wrapper's only.
set -u
SC=${1:?scenario name}; BIN=${2:?game binary}; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../../.." && pwd)
P=18797
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
case "$SC" in
  qa_lan_friend_codes) D=/home/claude/g3/hruns/lan-codes ;;
  qa_friend_from_played_with) D=/home/claude/g3/hruns/qa-online ;;
  *) echo "unknown scenario $SC"; exit 2 ;;
esac
LOG=$(mktemp)
pkill -f "fake/server.mjs --no-https --http-port $P" 2>/dev/null; sleep 1
(cd "$TREE/server" && GAME=smg REGISTER_POW_BITS=0 setsid nohup "$NODE" fake/server.mjs --no-https --http-port $P >"$LOG" 2>&1 &)
for _ in $(seq 1 300); do grep -q "http://127.0.0.1:$P" "$LOG" && break; sleep 0.5; done
grep -q "http://127.0.0.1:$P" "$LOG" || { cat "$LOG"; echo "fake service did not start"; pkill -f "fake/server.mjs --no-https --http-port $P"; exit 1; }
trap 'pkill -f "fake/server.mjs --no-https --http-port $P" 2>/dev/null; rm -f "$LOG"' EXIT
rm -rf "$D/host" "$D/guest"; mkdir -p "$D/host" "$D/guest"   # (only this scenario's own scratch folders)
# The GUEST gets its online account here, before the run: the Add Friend row on a player's page only exists once the OTHER console has announced
# its code, and a console makes its account only from that press (needAccount), so two fresh consoles can never start (friendscn, 14 Nov).
# The host makes its own account from its Add Friend press, in the run. Local fake only (the script refuses any other address).
GUEST_CODE=$(python3 -I "$HERE/seed_online_account.py" "http://127.0.0.1:$P" "$D/guest" Luigi) || { echo "seeding the guest account failed"; exit 1; }
echo "guest account seeded code=$GUEST_CODE"
export HARNESS_BIN=$BIN
/home/claude/g3/tools/gamerun.sh python3 "$HERE/run_scenario.py" "$HERE/scenarios/$SC.yaml" "$@"
echo "scenario $SC exit $?"

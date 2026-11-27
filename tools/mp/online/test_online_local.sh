#!/bin/bash
# Two online bots through the LOCAL fake of the online service (server/fake/server.mjs: the Worker's own code, plain http on 127.0.0.1):
# register, host a room, join by code, 20 s of verified reliable events + poses over the real NpNet link (UDP on 127.0.0.1), then the same
# with Play with Anyone (a public room found by quick match). Never contacts the live service.
#   tools/mp/online/test_online_local.sh <mp_bot built by build_bot_online.sh> [workdir]
# Exit 0 = every run passed (a guest that leaves first ends a 2-player host with HOST_LEFT = exit 2: that is expected and not judged).
set -u
root=$(cd "$(dirname "$0")/../../.." && pwd)
BOT=$(readlink -f "${1:?mp_bot}")
W=${2:-$(mktemp -d)}; mkdir -p "$W"; cd "$W" || exit 2
PORT=${FAKE_PORT:-$((18200 + $$ % 500))}
( cd "$root" && GAME=smg REGISTER_POW_BITS=0 SMG_FLAG_NEW_STAR=0 exec nice node server/fake/server.mjs --no-https --http-port "$PORT" ) > fake.log 2>&1 &
FP=$!; trap 'kill $FP 2>/dev/null' EXIT
for _ in $(seq 1 60); do curl -s "http://127.0.0.1:$PORT/v1/info" >/dev/null 2>&1 && break; sleep 0.25; done
U=http://127.0.0.1:$PORT; fail=0
one() {   # name, host-extra, guest-args...
  local n=$1 hx=$2; shift 2; rm -rf "$n"; mkdir -p "$n"
  { timeout 90 "$BOT" --online-host $hx --name Hosty --service $U --data-dir "$n/hd" --data-hash aa3ddccb --slot-wait 2 --max 2 --duration 40 --verify --quiet > "$n/h.log" 2>&1; } &
  local hp=$!
  for _ in $(seq 1 40); do grep -q "ROOM CODE:" "$n/h.log" && break; sleep 1; done
  local code; code=$(grep -o "ROOM CODE: [A-Z0-9]*" "$n/h.log" | head -1 | awk '{print $3}')
  [ -n "$code" ] || { echo "FAIL $n: the host got no room code"; fail=1; wait $hp; return; }
  sleep 2
  local args=("${@//@CODE@/$code}")
  timeout 90 "$BOT" "${args[@]}" --name Guesty --service $U --data-dir "$n/gd" --data-hash aa3ddccb --duration 20 --verify --quiet > "$n/g.log" 2>&1; local rc=$?
  wait $hp
  local v; v=$(grep -o "verify: sent [0-9]*, received [0-9]*, duplicates [0-9]*, gaps [0-9]*" "$n/g.log" | tail -1)
  if [ $rc = 0 ] && echo "$v" | grep -q "duplicates 0, gaps 0" && ! echo "$v" | grep -q "received 0,"; then echo "ok   $n (room $code; guest: $v)"; else echo "FAIL $n (guest exit $rc; $v)"; fail=1; fi
}
one by_code "" --online-join @CODE@
one quick "--visibility 2" --online-quick
exit $fail

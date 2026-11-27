#!/bin/bash
# run_all.sh - the whole seedwalk harvest, unattended: every leg of OUT/plan.json, one at a time, each inside an empty network namespace.
#   run_all.sh OUT                 (OUT made by `plan.py make --out OUT`)
# Environment (V3K and EBOOT required, as for leg.sh):
#   V3K=<emulator setup>  EBOOT=<candidate eboot.bin>  SEED=<candidate shadercache folder>  SAVE_TEMPLATE=<folder holding saves/>
#   NICE=10                the nice level of every leg (the emulator and its X server inherit it)
#   MIN_FREE_GB=2          stop cleanly (between legs, or by stopping the running leg) when OUT's file system has less free space
#   MAX_TRIES=2            runs of one leg at most (an interrupted run, by a build or by low disk, does not count)
#   RESUME_ROUNDS=2        rounds of `plan.py resume` (a follow-up leg for the stops an incomplete leg did not finish) after the main pass
#   POLL=30                seconds between checks (build, other emulator, disk, OUT/STOP) while waiting and while a leg runs
#   BUSY_PATTERN='build31\.sh'   a running process whose command line matches (pgrep -f) holds the harvest: a leg is not started, a running one is stopped
#                          and run again after it ended. Another emulator (a Vita3K or an Xvfb :77 that is not this run's) also holds it, before a leg starts.
#   NO_COLLECT=1           do not run collect.py at the end
# Resumable: run it again with the same OUT. Finished legs (legs/<id>/DONE for this eboot and seed) are skipped in a second; the try counts are kept in
# OUT/runall/tries/. `touch OUT/STOP` stops after the running leg; SIGTERM / Ctrl-C stops the running leg at once (its log is kept; it runs again next time).
# Progress: OUT/progress.txt (rewritten after every event), OUT/runall/events.log (appended), OUT/runall/legs.tsv (one line per leg run).
# Network: every leg runs under `unshare -n` (a new network namespace: only the loopback interface, which stays down; no route, no DNS), the same as the
# emu31b runs; the namespace is checked from inside before leg.sh starts (a leg whose namespace has any other interface is refused), and the leg gets a clean
# environment (no proxy variables, no tokens). The live service cannot be reached from a leg.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 1 ] || { sed -n 2,23p "$0"; exit 2; }
OUT=$(readlink -m "$1")
: "${V3K:?run_all.sh: V3K=<the headless Vita3K setup folder> is required}"
: "${EBOOT:?run_all.sh: EBOOT=<candidate eboot.bin> is required}"
V3K=$(readlink -m "$V3K"); EBOOT=$(readlink -m "$EBOOT")
NICE=${NICE:-10}; MIN_FREE_GB=${MIN_FREE_GB:-2}; MAX_TRIES=${MAX_TRIES:-2}; RESUME_ROUNDS=${RESUME_ROUNDS:-2}; POLL=${POLL:-30}
BUSY_PATTERN=${BUSY_PATTERN:-'build31\.sh'}
PY="python3 -I"
[ -f "$OUT/plan.json" ] || { echo "run_all.sh: $OUT/plan.json missing (plan.py make --out $OUT)"; exit 2; }
[ -f "$EBOOT" ] || { echo "run_all.sh: no eboot at $EBOOT"; exit 2; }
[ -x "$V3K/run.sh" ] || { echo "run_all.sh: no $V3K/run.sh"; exit 2; }
command -v unshare >/dev/null || { echo "run_all.sh: unshare is missing: legs must run in a network namespace"; exit 2; }
unshare -n true 2>/dev/null || { echo "run_all.sh: unshare -n fails here (needs root): no leg runs outside a network namespace"; exit 2; }
R=$OUT/runall
mkdir -p "$R/tries" "$R/out"
exec 8>"$R/lock"
flock -n 8 || { echo "run_all.sh: another run_all.sh holds $R/lock"; exit 3; }

EVENTS=()
T_START=$(date +%s)
STATE="starting"
CUR=""; CUR_T0=0; CUR_EST=0
now() { date '+%Y-%m-%d %H:%M:%S'; }
event() { local m="$(now) $*"; echo "$m" >> "$R/events.log"; echo "$m"; EVENTS+=("$m"); [ ${#EVENTS[@]} -gt 15 ] && EVENTS=("${EVENTS[@]: -15}"); progress; }
free_bytes() { df -P -B1 "$OUT" | awk 'NR == 2 { print $4 }'; }
low_disk() { [ "$(free_bytes)" -lt $((MIN_FREE_GB * 1024 * 1024 * 1024)) ]; }
busy_build() { pgrep -f -- "$BUSY_PATTERN" 2>/dev/null | grep -v -x "$$" | head -1; }
other_emulator() { pgrep -f '[V]ita3K' 2>/dev/null | grep -v -x "$$" | head -1; pgrep -f 'Xvfb :77 ' 2>/dev/null | head -1; }   # (any Vita3K process, as pgrep -f Vita3K finds it)

# the legs in order, with what the progress line needs: id kind est_s done(0|1) has_followup(0|1)
leg_table() {
    $PY - "$OUT" <<'EOF'
import json, re, sys
from pathlib import Path
out = Path(sys.argv[1]); plan = json.loads((out / "plan.json").read_text()); legs = plan["legs"]
for l in legs:
    s = json.loads((out / "legs" / l / "spec.json").read_text())
    fol = any(re.fullmatch(re.escape(l) + r"-r\d+", x) for x in legs)
    print(l, s["kind"], s.get("est_wall_s", 0), int((out / "legs" / l / "DONE").exists()), int(fol))
EOF
}
tries() { cat "$R/tries/$1" 2>/dev/null || echo 0; }

progress() {
    local tab done=0 total=0 left_est=0 est_done=0 wall_done=0 line id kind est d fol   # (all local: bash scope is dynamic, the callers have an id too)
    tab=$(leg_table 2>/dev/null)
    while read -r id kind est d fol; do
        [ -n "$id" ] || continue
        total=$((total + 1))
        if [ "$d" = 1 ]; then done=$((done + 1)); est_done=$((est_done + est))
        elif [ "$fol" = 0 ] && [ "$(tries "$id")" -lt "$MAX_TRIES" ]; then left_est=$((left_est + est)); fi
    done <<< "$tab"
    wall_done=$(awk -F'\t' '$6 == "complete" { s += $4 } END { print s + 0 }' "$R/legs.tsv" 2>/dev/null)
    local ratio="1.00"; [ "$est_done" -gt 0 ] && [ "$wall_done" -gt 0 ] && ratio=$(awk -v a="$wall_done" -v b="$est_done" 'BEGIN { printf "%.2f", a / b }')
    local eta=$(awk -v e="$left_est" -v r="$ratio" 'BEGIN { printf "%.1f", e * r / 3600 }')
    {
        echo "seedwalk run_all: $OUT"
        echo "updated: $(now)   started: $(date -d @"$T_START" '+%Y-%m-%d %H:%M:%S')   elapsed: $(( ($(date +%s) - T_START) / 60 )) min"
        echo "state: $STATE"
        [ -n "$CUR" ] && echo "current leg: $CUR, running $(( ($(date +%s) - CUR_T0) / 60 )) min (planned ~$((CUR_EST / 60)) min)"
        echo "legs: $done of $total done; left (planned, scaled by the walls seen so far x$ratio): ~$eta h"
        echo "eboot: $EBOOT (sha256 ${EBSHA:0:16})   seed: ${SEED:-$V3K/pkgsc/shadercache}   emulator: $V3K"
        echo "free disk on OUT: $(( $(free_bytes) / 1024 / 1024 )) MiB (stop below $MIN_FREE_GB GiB)"
        echo "network: every leg in its own namespace (unshare -n: loopback only, down; checked inside before the leg)"
        echo
        echo "last events:"
        for line in "${EVENTS[@]}"; do echo "  $line"; done
        echo
        echo "legs.tsv (leg, try, start, wall s, leg.sh rc, result):"
        tail -n 12 "$R/legs.tsv" 2>/dev/null | sed 's/^/  /'
    } > "$OUT/progress.txt.tmp" && mv -f "$OUT/progress.txt.tmp" "$OUT/progress.txt"
}

LPID=""
stop_leg() {   # stop the running leg's whole session (run.sh's trap stops the emulator and its X server)
    [ -n "$LPID" ] || return 0
    pkill -TERM -s "$LPID" 2>/dev/null
    for _ in $(seq 1 60); do pgrep -s "$LPID" >/dev/null 2>&1 || break; sleep 1; done
    pkill -KILL -s "$LPID" 2>/dev/null
    wait "$LPID" 2>/dev/null
    LPID=""
}
on_signal() { STATE="stopped by a signal (the running leg was stopped; it runs again next time)"; stop_leg; event "stopped by a signal"; exit 130; }
trap on_signal INT TERM

# wait until nothing holds the harvest; returns 1 when the run must stop (OUT/STOP, low disk)
wait_clear() {
    local said=""
    while :; do
        [ -e "$OUT/STOP" ] && { STATE="stopped: $OUT/STOP exists (remove it and start run_all.sh again to go on)"; event "stop file found"; return 1; }
        if low_disk; then STATE="stopped: low disk ($(( $(free_bytes) / 1024 / 1024 )) MiB free, below $MIN_FREE_GB GiB): free space and start run_all.sh again"; event "low disk: stopped"; return 1; fi
        local b o; b=$(busy_build); o=$(other_emulator)
        if [ -z "$b" ] && [ -z "$o" ]; then [ -n "$said" ] && event "clear again: going on"; return 0; fi
        local why=""; [ -n "$b" ] && why="a build is running (pid $b: $(ps -o args= -p "$b" 2>/dev/null | cut -c1-80))"; [ -n "$o" ] && why="${why:+$why; }another emulator is running (pid $o)"
        if [ "$said" != "$why" ]; then STATE="waiting: $why"; event "waiting: $why"; said=$why; fi
        sleep "$POLL"
    done
}

# one leg, in a network namespace; sets RES (complete | incomplete | failed rc=N | interrupted: build | interrupted: low disk | interrupted: stop)
run_leg() {
    local id=$1 est=$2 t0 rc
    CUR=$id; CUR_T0=$(date +%s); CUR_EST=$est; STATE="running $id"
    local try=$(( $(tries "$id") + 1 ))
    event "leg $id: start (try $try of $MAX_TRIES, planned ~$((est / 60)) min)"
    t0=$(date +%s)
    nice -n "$NICE" setsid unshare -n -- env -i PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" LANG=C.UTF-8 \
        V3K="$V3K" EBOOT="$EBOOT" ${SEED:+SEED="$SEED"} ${SAVE_TEMPLATE:+SAVE_TEMPLATE="$SAVE_TEMPLATE"} \
        bash -c 'python3 -I -c "import socket, sys; n = [x for _, x in socket.if_nameindex()]; print(\"netns: interfaces\", n); sys.exit(0 if n == [\"lo\"] else 1)" \
                 || { echo "run_all: the network namespace has more than the loopback: the leg is refused"; exit 9; }
                 exec bash "$0" "$1" "$2"' "$HERE/leg.sh" "$OUT" "$id" > "$R/out/$id.try$try.txt" 2>&1 &
    LPID=$!
    RES=""
    while kill -0 "$LPID" 2>/dev/null; do
        sleep "$POLL" & wait $! 2>/dev/null
        kill -0 "$LPID" 2>/dev/null || break
        if [ -n "$(busy_build)" ]; then RES="interrupted: build"; break; fi
        if low_disk; then RES="interrupted: low disk"; break; fi
        progress
    done
    if [ -n "$RES" ]; then
        event "leg $id: $RES: stopping it"
        stop_leg
        rc=-1
    else
        wait "$LPID"; rc=$?; LPID=""
        echo "$try" > "$R/tries/$id"
        case $rc in
            0) RES=complete;;
            4) RES=incomplete;;
            9) RES="failed rc=9 (network namespace check)";;
            *) RES="failed rc=$rc";;
        esac
    fi
    local wall=$(( $(date +%s) - t0 ))
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$try" "$(date -d @"$t0" '+%m-%d %H:%M')" "$wall" "$rc" "$RES" >> "$R/legs.tsv"
    CUR=""
    event "leg $id: $RES after $((wall / 60)) min $((wall % 60)) s (log: runall/out/$id.try$try.txt)"
}

EBSHA=$(sha256sum "$EBOOT" | cut -d' ' -f1)
event "run_all start: plan $($PY -c 'import json,sys;print(json.load(open(sys.argv[1]))["fingerprint"])' "$OUT/plan.json"), eboot ${EBSHA:0:16}"

pass() {   # one pass over the legs not done; returns 1 when the run must stop
    local id kind est d fol
    while read -r id kind est d fol; do
        [ -n "$id" ] || continue
        [ "$d" = 1 ] && continue          # (leg.sh would skip it too; this saves its seed checksum)
        [ "$fol" = 1 ] && continue        # (its missing stops went to its follow-up leg)
        [ "$(tries "$id")" -ge "$MAX_TRIES" ] && continue
        while :; do
            wait_clear || return 1
            run_leg "$id" "$est"
            case "$RES" in
                "interrupted: build") continue;;                 # (run again once the build ended: wait_clear waits for it)
                "interrupted: low disk") STATE="stopped: low disk during $id (it runs again next time)"; progress; return 1;;
            esac
            break
        done
        if [ -e "$OUT/STOP" ]; then STATE="stopped: $OUT/STOP exists"; event "stop file found"; return 1; fi
    done < <(leg_table)
    return 0
}

stopped=0
pass || stopped=1
round=0
while [ $stopped = 0 ] && [ $round -lt "$RESUME_ROUNDS" ]; do
    round=$((round + 1))
    made=$($PY "$HERE/plan.py" resume "$OUT" 2>&1 | tail -1)
    event "resume round $round: $made"
    case "$made" in *"none"*) break;; esac
    pass || stopped=1
done
if [ $stopped = 1 ]; then progress; exit 5; fi

left=$(leg_table | awk -v R="$R" '$4 == 0 && $5 == 0 { print $1 }' | tr '\n' ' ')
if [ "${NO_COLLECT:-0}" != 1 ]; then
    STATE="collecting"; event "collect.py collect"
    $PY "$HERE/collect.py" collect "$OUT" > "$R/collect.txt" 2>&1; crc=$?
    event "collect.py: exit $crc ($(tail -1 "$R/collect.txt" | cut -c1-160)); report runall/collect.txt, harvest in $OUT/harvest"
fi
STATE="finished$([ -n "$left" ] && echo "; legs not complete: $left")"
event "run_all finished"
exit 0

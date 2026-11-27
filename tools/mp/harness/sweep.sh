#!/bin/bash
# Runs a list of scenarios one at a time through the machine-wide game-run queue and writes a status table.
#   sweep.sh [-b GAME_BINARY] [-l LIST_FILE] [-o STATUS_FILE] [-d RUN_PARENT] [scenario ...]
#     scenarios   names (a file of tools/mp/harness/scenarios without .yaml) or paths; none: the list file
#     -l          list file, one scenario per line, '#' comments (default: sweep_order.txt = the qa-func audit's sweep order, 21 runs)
#     -o          status file (default /home/claude/g3/hruns/sweep-status.tsv), rewritten after every scenario
#     -d          parent folder of the run folders (default $HARNESS_OUT/<date> as run_scenario.py)
#     -b          the game binary (HARNESS_BIN)
# Per scenario:
#   * waits while the 1-minute load average is above SWEEP_MAX_LOAD (default 2.5), polling every 20 s (SWEEP_LOAD_WAIT_MAX seconds at most per
#     scenario, default 0 = no limit), because a starved game gives INVALID, not an answer;
#   * runs `gamerun.sh run_scenario.py <scenario>` (the strict lane); a SKIP (feature or file missing) is recorded and not repeated;
#   * an INVALID verdict (the machine starved the game) is run again, up to SWEEP_RERUNS more times (default 2), waiting for the load again;
#   * a PASS deletes its run folder (disk); a FAIL / INVALID keeps it and the table names it.
# The status file is a tab-separated table: scenario, verdict (PASS FAIL INVALID SKIP ERROR, PENDING / RUNNING while the sweep is under way),
# seconds (wall, all attempts), speed (the slowest process's game seconds per wall second), failed rules (hard ones, ';'-joined), attempts, folder.
# Environment: SWEEP_GAMERUN (default /home/claude/g3/tools/gamerun.sh, '' = no queue), SWEEP_RUNNER (default run_scenario.py next to this file).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATUS=${SWEEP_STATUS:-/home/claude/g3/hruns/sweep-status.tsv}
LIST=$HERE/sweep_order.txt; OUTDIR=""; BIN=""
while getopts "b:l:o:d:" o; do
  case $o in b) BIN=$OPTARG;; l) LIST=$OPTARG;; o) STATUS=$OPTARG;; d) OUTDIR=$OPTARG;; *) exit 2;; esac
done
shift $((OPTIND - 1))
if [ $# -gt 0 ]; then names=("$@"); else mapfile -t names < <(sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$LIST" | grep -v '^$'); fi
[ -n "$BIN" ] && export HARNESS_BIN=$BIN
GAMERUN=${SWEEP_GAMERUN-/home/claude/g3/tools/gamerun.sh}
RUNNER=${SWEEP_RUNNER:-$HERE/run_scenario.py}
MAXLOAD=${SWEEP_MAX_LOAD:-2.5}; RERUNS=${SWEEP_RERUNS:-2}; LOADMAX=${SWEEP_LOAD_WAIT_MAX:-0}
mkdir -p "$(dirname "$STATUS")"
declare -A V S SP F N D path
order=()
write_status() {   # the whole table, atomically
  local n
  { printf 'scenario\tverdict\tseconds\tspeed\tfailed_rules\tattempts\tfolder\n'
    for n in "${order[@]}"; do printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "${V[$n]:-PENDING}" "${S[$n]:--}" "${SP[$n]:--}" "${F[$n]:--}" "${N[$n]:-0}" "${D[$n]:--}"; done
  } > "$STATUS.tmp" && mv "$STATUS.tmp" "$STATUS"
}
wait_for_load() {
  local t0=$SECONDS l
  while :; do
    l=$(cut -d' ' -f1 /proc/loadavg)
    awk -v l="$l" -v m="$MAXLOAD" 'BEGIN{exit !(l+0 <= m+0)}' && return 0
    if [ "$LOADMAX" != 0 ] && [ $((SECONDS - t0)) -ge "$LOADMAX" ]; then echo "sweep: load $l still above $MAXLOAD after ${LOADMAX}s: going on" >&2; return 0; fi
    echo "sweep: load $l > $MAXLOAD, waiting" >&2
    sleep 20
  done
}
# one attempt: sets $verdict, $folder; stdout of the runner goes to $log
attempt() {
  local sc=$1 log=$2 cmd=(python3 "$RUNNER")
  [ -n "$OUTDIR" ] && cmd+=(-o "$OUTDIR")
  cmd+=("$sc")
  if [ -n "$GAMERUN" ]; then "$GAMERUN" "${cmd[@]}" > "$log" 2>&1; else "${cmd[@]}" > "$log" 2>&1; fi
  # the runner's summary row: "<VERDICT> <name> <why> <folder>" after "== scenarios"
  row=$(sed -n '/^== scenarios/,$p' "$log" | sed 1d | grep -E '^(PASS|FAIL|INVALID|SKIP|DRY) ' | head -1)
  verdict=${row%% *}
  folder=$(printf '%s' "$row" | grep -o '/[^ ]*$' || true)
  [ -z "$verdict" ] && verdict=ERROR
}
details() {   # <folder> -> "speed<TAB>failed rules" from its verdict.json
  python3 - "$1" <<'PY'
import json, sys, os
f = os.path.join(sys.argv[1], 'verdict.json')
try:
    j = json.load(open(f))
except Exception:
    print('-\t-'); sys.exit()
sp = [p['speed'] for p in (j.get('machine') or {}).get('procs', []) if p.get('speed') is not None]
bad = [r['rule'] for r in j.get('results', []) if not r.get('ok') and not r.get('soft')]
print('%s\t%s' % ('%.2f' % min(sp) if sp else '-', ' ; '.join(b.replace('\t', ' ') for b in bad)[:600] or '-'))
PY
}
for name in "${names[@]}"; do
  sc=$name; [ -f "$sc" ] || sc=$HERE/scenarios/$name.yaml
  n=$(basename "$sc" .yaml); order+=("$n"); path[$n]=$sc
done
write_status
for n in "${order[@]}"; do
  V[$n]=RUNNING; write_status
  t0=$SECONDS; tries=0; speed=-; failed=-; folder=-
  while :; do
    wait_for_load
    tries=$((tries + 1))
    log=$(mktemp /tmp/sweep-XXXXXX.log)
    echo "sweep: $n (attempt $tries)" >&2
    attempt "${path[$n]}" "$log"
    if [ -n "$folder" ] && [ -d "$folder" ]; then IFS=$'\t' read -r speed failed < <(details "$folder"); else speed=-; failed=-; folder=-; fi
    [ "$verdict" = ERROR ] && failed="the runner printed no verdict: $(tail -n 3 "$log" | tr '\n\t' '  ' | cut -c1-300)"
    rm -f "$log"
    if [ "$verdict" = INVALID ] && [ $((tries - 1)) -lt "$RERUNS" ]; then
      echo "sweep: $n INVALID (the machine starved the game): again" >&2
      continue
    fi
    break
  done
  if [ "$verdict" = PASS ] && [ "$folder" != - ]; then rm -rf "$folder"; folder="(deleted)"; fi
  V[$n]=$verdict; S[$n]=$((SECONDS - t0)); SP[$n]=$speed; F[$n]=$failed; N[$n]=$tries; D[$n]=$folder
  write_status
done
column -t -s$'\t' "$STATUS" 2>/dev/null || cat "$STATUS"
fails=$(awk -F'\t' 'NR>1 && $2!="PASS" && $2!="SKIP"' "$STATUS" | wc -l)
echo "sweep: ${#order[@]} scenarios, $fails not PASS/SKIP (status: $STATUS)"
[ "$fails" = 0 ]

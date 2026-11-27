#!/bin/bash
# Several headless game processes (host, guest, ...) and optional mp_bot clients started together on 127.0.0.1,
# each in its own run folder under one pair folder, then a merged timeline.
#
#   run_pair.sh [options]
#     -n NAME            pair folder name (default pair-<time>)
#     -o DIR             parent folder (default $HARNESS_OUT/<date>)
#     -s SECONDS         run length of every game process [120]
#     -e KEY=VALUE       environment for every game process (repeatable)
#     -p LABEL[:ENVFILE[:DELAY_S[:SECONDS]]]
#                        a game process (repeatable; default: -p host -p guest). ENVFILE: KEY=VALUE lines for it,
#                        DELAY_S: start that many seconds after the others, SECONDS: its own run length
#     -P LABEL=KEY=VALUE environment for one process (repeatable), e.g. -P host=SMG_MP_AUTOHOST=together
#     -b 'LABEL CMD...'  an extra process (mp_bot), started with the same link environment; it gets HARNESS_RUN_DIR and
#                        HARNESS_SECONDS and is stopped (SIGTERM) after SECONDS+30 s
#     -L                 no automatic link environment (see below)
#     -g                 every game process under gdb
#     -l PROFILE|FILE.json  bad-network relay between the consoles (link_relay.py): a profile name (home_wifi, bad_wifi, hotspot, outage10, ...)
#                        or a JSON/YAML config file; HARNESS_LINK_PROFILE does the same from the environment
#     -t SECONDS         hard timeout of every game process (run_one.sh -t; default seconds*3+120 under the game clock)
#
# Link environment (unless -L), process i = 0,1,2... in the order given (games first, then bots):
#   SMG_LINK_PORT=<base>+i (base: the first free, locked block of 41110, 41120, ...; HARNESS_LINK_BASE forces it), SMG_LINK_BCAST=127.0.0.1:<every port of the set>,
#   SMG_LINK_VIP=10.77.0.<i+1>, SMG_LINK_NAME=<label>  (net/src/vlink_udp.cpp: POSIX UDP backend, own port skipped)
#   and the same as SMG_MP_UDP_PORT / SMG_MP_UDP_BCAST / SMG_MP_UDP_VIP (0x0a4d00<i+1>) / SMG_MP_NAME (core's mp_transport_vlink.cpp)
# Output: <pair>/<label>/ (as run_one.sh), <pair>/<label>.out, <pair>/timeline.txt (merged, filtered),
#   <pair>/timeline-all.txt (every line), <pair>/pair.json. Exit 0 when every game run is OK (run_one.sh's verdict).
set -u
source "$(dirname "$0")/common.sh"
# (the whole script is one function: bash reads it completely before running, so editing this file during a run is safe)
main() {

harness_warm "$HARNESS_BIN"
name=pair-$(date +%H%M%S)-$$
parent=$HARNESS_OUT/$(date +%Y%m%d)
seconds=120
link=1
linkcfg=
gdb=
declare -a common=() procs=() perenv=() bots=()
tmo=
while getopts "n:o:s:e:p:P:b:Lgt:l:" opt; do
  case $opt in
    n) name=$OPTARG ;;
    o) parent=$OPTARG ;;
    s) seconds=$OPTARG ;;
    e) common+=("$OPTARG") ;;
    p) procs+=("$OPTARG") ;;
    P) perenv+=("$OPTARG") ;;
    b) bots+=("$OPTARG") ;;
    L) link=0 ;;
    g) gdb=-g ;;
    t) tmo=$OPTARG ;;
    l) linkcfg=$OPTARG ;;
    *) sed -n '2,25p' "$0"; exit 2 ;;
  esac
done
[ ${#procs[@]} -eq 0 ] && procs=(host guest)
[ -z "$linkcfg" ] && linkcfg=${HARNESS_LINK_PROFILE:-}
pair=$parent/$name
[ -e "$pair" ] && harness_die "pair folder exists: $pair"
mkdir -p "$pair" && pair=$(cd "$pair" && pwd)
go=$pair/.go
total=$(( ${#procs[@]} + ${#bots[@]} ))
# A block of UDP ports of our own (common.sh harness_port_lease: one flock'ed lease file per port under $HARNESS_PORT_DIR, shared by every
# agent, held until this script ends, a dead holder's lease is free again). With a link relay (-l) the block also holds the relay's
# sockets: relay_ports_needed in link_relay.py (n for cone NAT, n*(n-1) for symmetric NAT) after the n console ports.
# HARNESS_LINK_BASE forces a base (it is still leased; a busy one is only a warning, the old workaround).
HARNESS_LEASE_LABEL=$name
need=$total
if [ -n "$linkcfg" ]; then need=$(( total + $("$HARNESS_PY" "$HARNESS_DIR/link_relay.py" --ports-needed "$total" "$linkcfg") )); fi
dport_off=$need; need=$(( need + 1 ))   # one more leased port, past the console and relay ports: @DIRECT_PORT@ in any -e/-P/-b value (play by address: SMG_MP_DIRECT_PORT)
if [ -n "${HARNESS_LINK_BASE:-}" ]; then
  base=$HARNESS_LINK_BASE
  harness_port_lease_at "$base" "$need" || echo "run_pair: warning: forced HARNESS_LINK_BASE=$base is leased by another run" >&2
else
  harness_port_lease "$need" || harness_die "no free UDP port block of $need in 41110..41999"
  base=$LEASE_BASE
fi
trap '[ -n "${relay_pid:-}" ] && kill -TERM "$relay_pid" 2>/dev/null' EXIT
echo "$base" > "$pair/link-base.txt"
dport=$(( base + dport_off )); echo "$dport" > "$pair/direct-port.txt"
common=("${common[@]//@DIRECT_PORT@/$dport}"); perenv=("${perenv[@]//@DIRECT_PORT@/$dport}"); bots=("${bots[@]//@DIRECT_PORT@/$dport}")
loadavg() { cut -d' ' -f1-3 /proc/loadavg; }
load_start=$(loadavg)
bcast=$(for ((i = 0; i < total; i++)); do printf '127.0.0.1:%d,' $((base + i)); done); bcast=${bcast%,}

# Bad-network relay (-l / HARNESS_LINK_PROFILE): console i keeps port base+i but broadcasts to the relay's sockets, not to the other consoles
declare -a relay_bcast=()
relay_pid=
if [ -n "$linkcfg" ] && [ "$link" = 1 ]; then
  names=
  for spec in "${procs[@]}"; do names+="${spec%%:*},"; done
  for spec in "${bots[@]}"; do names+="${spec%% *},"; done
  names=${names%,}
  while read -r idx list; do relay_bcast[$idx]=$list; done < <("$HARNESS_PY" "$HARNESS_DIR/link_relay.py" --layout "$total" "$base" "$linkcfg")
  "$HARNESS_PY" "$HARNESS_DIR/link_relay.py" --base "$base" --names "$names" --config "$linkcfg" --out "$pair" --watch-dir "$pair" \
      --ready-file "$pair/.relay-ready" > "$pair/link-relay.out" 2>&1 &
  relay_pid=$!
  for _ in $(seq 1 100); do [ -e "$pair/.relay-ready" ] && break; sleep 0.1; done
  [ -e "$pair/.relay-ready" ] || harness_die "link relay did not start: $(cat "$pair/link-relay.out")"
  echo "run_pair: link relay pid $relay_pid, config $linkcfg"
fi
link_env() {  # index label
  [ "$link" = 1 ] || return 0
  local bc=${relay_bcast[$1]:-$bcast}
  printf 'SMG_LINK_PORT=%d\nSMG_LINK_BCAST=%s\nSMG_LINK_VIP=10.77.0.%d\nSMG_LINK_NAME=%s\n' $((base + $1)) "$bc" $(($1 + 1)) "$2"
  # (the core agent's transport reads SMG_MP_UDP_*; VIP as a number: 10.77.0.x = 0x0a4d00xx)
  printf 'SMG_MP_UDP_PORT=%d\nSMG_MP_UDP_BCAST=%s\nSMG_MP_UDP_VIP=0x0a4d00%02x\nSMG_MP_NAME=%s\n' $((base + $1)) "$bc" $(($1 + 1)) "$2"
}

declare -a pids=() labels=() delays=()
i=0
for spec in "${procs[@]}"; do
  IFS=: read -r label envfile delay secs <<< "$spec"
  secs=${secs:-$seconds}
  delay=${delay:-0}
  ef=$pair/$label.env
  {
    for kv in "${common[@]}"; do echo "$kv"; done
    [ -n "${envfile:-}" ] && grep -v '^[[:space:]]*#' "$envfile" | grep '='
    for kv in "${perenv[@]}"; do [[ "$kv" == "$label="* ]] && echo "${kv#"$label="}"; done
  } > "$ef.tmp"
  # a continuing process (HARNESS_CONTINUE=<label>) is the same console: it keeps <label>'s link port, VIP and name
  cont=$(grep -m1 '^HARNESS_CONTINUE=' "$ef.tmp" | cut -d= -f2-)
  ident_i=$i; ident_label=$label
  if [ -n "$cont" ]; then
    for k in "${!labels[@]}"; do [ "${labels[$k]}" = "$cont" ] && ident_i=$k && ident_label=$cont; done
  fi
  { link_env $ident_i "$ident_label"; cat "$ef.tmp"; } > "$ef"
  rm -f "$ef.tmp"
  # HARNESS_AFTER=<label> in a process's env: start once <label> has ended (then DELAY_S more); HARNESS_CONTINUE=<label>:
  # start from <label>'s saves and multiplayer root (run_one.sh -C), e.g. the same console restarting after a kill -9
  after=$(grep -m1 '^HARNESS_AFTER=' "$ef" | cut -d= -f2-)
  [ -n "$after" ] && [ "$delay" = 0 ] && delay=0.1
  # HARNESS_START_ON="<label>|<PCRE>|<delay s>" in a process's env: start once process <label> has printed a line matching <PCRE> (+ delay s; \d \w \S work; the regex cannot contain a "|": it is the field separator): a late
  # joiner that arrives at a mark of the other console (e.g. the host's move to a galaxy = the host is loading) instead of at a fixed time.
  starton=$(grep -m1 '^HARNESS_START_ON=' "$ef" | cut -d= -f2-)
  [ -n "$starton" ] && [ "$delay" = 0 ] && delay=0.1
  (
    if [ "$delay" != 0 ]; then
      while [ ! -e "$go" ]; do sleep 0.1; done
      if [ -n "$after" ]; then while [ ! -e "$pair/$after/result.json" ]; do sleep 0.5; done; fi
      if [ -n "$starton" ]; then
        IFS='|' read -r so_label so_rx so_delay <<< "$starton"
        until harness_start_on_match "$so_rx" "$pair/$so_label/console.log"; do
          [ -e "$pair/$so_label/result.json" ] && { echo "harness: HARNESS_START_ON: $so_label ended without a line matching $so_rx" >&2; break; }
          sleep 0.2
        done
        [ -n "${so_delay:-}" ] && sleep "$so_delay"
      fi
      sleep "$delay"
    fi
    exec "$HARNESS_DIR/run_one.sh" -n "$label" -o "$pair" -s "$secs" -f "$ef" -w "$go" ${tmo:+-t "$tmo"} $gdb ${cont:+-C "$pair/$cont"}
  ) > "$pair/$label.out" 2>&1 &
  pids+=($!); labels+=("$label"); delays+=("$delay")
  i=$((i + 1))
done
declare -a botpids=()
for spec in "${bots[@]}"; do
  read -r label cmdline <<< "$spec"
  mkdir -p "$pair/$label"
  { link_env $i "$label"; echo "HARNESS_RUN_DIR=$pair/$label"; echo "HARNESS_SECONDS=$seconds"; } > "$pair/$label.env"
  (
    while [ ! -e "$go" ]; do sleep 0.1; done
    # shellcheck disable=SC2086
    exec "$HARNESS_PY" "$HARNESS_DIR/supervise.py" --run-dir "$pair/$label" --env-file "$pair/$label.env" \
      --timeout $((seconds + 30)) --name "$label" -- $cmdline
  ) > "$pair/$label.out" 2>&1 &
  botpids+=($!)
  i=$((i + 1))
done

# every run_one.sh has copied its saves and snapshotted the binary when its .ready exists; then start them all at once
for k in "${!labels[@]}"; do
  label=${labels[$k]}
  [ "${delays[$k]}" != 0 ] && continue
  for _ in $(seq 1 600); do [ -e "$pair/$label/.ready" ] && break; [ -e "$pair/$label.out" ] && grep -q 'harness:' "$pair/$label.out" && break; sleep 0.1; done
done
sleep 0.5
date +%s%3N > "$go"
echo "run_pair: ${#labels[@]} game process(es) ${labels[*]} + ${#bots[@]} bot(s) started in $pair"

rc=0
for k in "${!pids[@]}"; do
  wait "${pids[$k]}" || rc=1
  tail -1 "$pair/${labels[$k]}.out"
done
for p in "${botpids[@]}"; do wait "$p"; done
if [ -n "$relay_pid" ]; then kill -TERM "$relay_pid" 2>/dev/null; wait "$relay_pid" 2>/dev/null; echo "run_pair: link relay stats $pair/link-stats.json"; fi
"$HARNESS_PY" - "$pair/load.json" "$load_start" "$(loadavg)" <<'PY'
import json, sys
f = lambda s: [float(x) for x in s.split()]
json.dump({'start': f(sys.argv[2]), 'end': f(sys.argv[3])}, open(sys.argv[1], 'w'))
PY
"$HARNESS_PY" "$HARNESS_DIR/merge_timeline.py" "$pair" -o "$pair/timeline.txt" > /dev/null
"$HARNESS_PY" "$HARNESS_DIR/merge_timeline.py" "$pair" --all -o "$pair/timeline-all.txt" > /dev/null
"$HARNESS_PY" - "$pair" <<'PY'
import json, os, sys
pair = sys.argv[1]
procs = {}
for d in sorted(os.listdir(pair)):
    s = os.path.join(pair, d, 'summary.json')
    r = os.path.join(pair, d, 'result.json')
    if os.path.exists(s): procs[d] = json.load(open(s))
    elif os.path.exists(r): procs[d] = json.load(open(r))
starts = [p['start_ms'] for p in procs.values() if p.get('start_ms')]
ends = [p['end_ms'] for p in procs.values() if p.get('end_ms')]
out = {'pair': pair, 'processes': procs, 'wall_s': round((max(ends) - min(starts)) / 1000, 1) if starts and ends else None}
json.dump(out, open(os.path.join(pair, 'pair.json'), 'w'), indent=1)
for k, v in procs.items():
    if v.get('mem_line'): print(f"run_pair: {k} {v['mem_line']}")
print(f"run_pair: wall {out['wall_s']} s; peak RSS " + ', '.join(f"{k} {v.get('peak_hwm_kb', 0) // 1024} MiB" for k, v in procs.items()))
PY
echo "run_pair: timeline $pair/timeline.txt"
exit $rc
}
main "$@"; exit $?

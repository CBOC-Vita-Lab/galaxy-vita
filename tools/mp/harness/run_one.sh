#!/bin/bash
# One headless desktop game run in a fresh run folder.
#
#   run_one.sh [options] [-- extra game args]
#     -n NAME        run name (folder name; default one-<time>)
#     -o DIR         parent folder (default $HARNESS_OUT/<date>); the run folder is DIR/NAME and must not exist
#     -s SECONDS     game run length (main.cpp's <seconds>; game seconds under PETARI_GAME_CLOCK=1)  [60]
#     -e KEY=VALUE   environment for the game (repeatable; overrides the headless defaults of common.sh)
#     -f FILE        environment file (KEY=VALUE lines; # comments), applied before -e
#     -i SCRIPT      PETARI_INPUT timeline (same as -e PETARI_INPUT=SCRIPT); "@name[+S][,more entries]" reads inputs/<name>.txt
#                    (or a path; entries separated by commas or lines, # comments), shifted S seconds later, then the rest
#     -t SECONDS     hard timeout (default: seconds+150, or 3*seconds+120 under PETARI_GAME_CLOCK=1)
#     -S DIR         saves to copy into the run (default $HARNESS_SAVES0); -S none = start with no save
#     -g             run under gdb (batch): a crash prints every thread's backtrace into console.log
#     -x             run under xvfb-run (not needed with EGL_PLATFORM=surfaceless; kept for experiments)
#     -C RUNDIR      continue: start from that run's saves/ and mp/ (the SP save and the multiplayer root, e.g. after a kill)
#     -w             wait-start file: the process starts only once this file exists (run_pair.sh starts processes together)
#
# Output (run folder): console.log (every stdout/stderr line with an epoch-ms stamp), game.log (the port log), shots/frame_NNNN.png,
#   saves/ (a copy of the test save), save-before.json / save-after.json / save-guard.json, proc.csv (RSS/CPU every 0.5 s),
#   result.json (exit, timing, peak RSS, CPU), crash.txt (symbolized, when it crashed), env.txt, cmd.txt, summary.json.
# Exit status: 0 the game ended by itself with status 0, no crash, save guard OK; 1 otherwise; 2 usage / setup error.
set -u
source "$(dirname "$0")/common.sh"
# (the whole script is one function: bash reads it completely before running, so editing this file during a run is safe)
main() {

name=one-$(date +%H%M%S)-$$
parent=$HARNESS_OUT/$(date +%Y%m%d)
seconds=60
timeout=
envfile=
saves=$HARNESS_SAVES0
gdb=0
xvfb=${HARNESS_XVFB:-0}
waitfile=
continue_from=
declare -a envs=()
while getopts "n:o:s:e:f:i:t:S:gxw:C:" opt; do
  case $opt in
    n) name=$OPTARG ;;
    o) parent=$OPTARG ;;
    s) seconds=$OPTARG ;;
    e) envs+=("$OPTARG") ;;
    f) envfile=$OPTARG ;;
    i) envs+=("PETARI_INPUT=$OPTARG") ;;
    t) timeout=$OPTARG ;;
    S) saves=$OPTARG ;;
    g) gdb=1 ;;
    x) xvfb=1 ;;
    w) waitfile=$OPTARG ;;
    C) continue_from=$OPTARG ;;
    *) sed -n '2,24p' "$0"; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[[ "$seconds" =~ ^[1-9][0-9]*$ ]] || harness_die "bad seconds '$seconds'"
[[ "$name" =~ ^[A-Za-z0-9_.-]+$ ]] || harness_die "bad name '$name'"
run=$parent/$name
[ -e "$run" ] && harness_die "run folder exists: $run"
mkdir -p "$run/shots" "$run/saves" || harness_die "cannot make $run"
run=$(cd "$run" && pwd)

# --- environment: headless defaults < env file < -e
{
  harness_base_env
  echo "PETARI_SHADER_CACHE=$run/shader-cache"
  echo "PETARI_FRAMETIMES=$run/frametimes.txt"
  [ -n "$envfile" ] && grep -v '^[[:space:]]*#' "$envfile" | grep '='
  for kv in "${envs[@]}"; do echo "$kv"; done
} > "$run/env.raw"
# last value of each key wins; "@name" input timelines are expanded from inputs/
marks_ok=0
if [ -n "${HARNESS_BIN:-}" ] && grep -q "desktop: input mark" "$HARNESS_BIN" 2>/dev/null; then marks_ok=1; fi
"$HARNESS_PY" - "$run/env.raw" "$run/env.txt" "$HARNESS_DIR/inputs" "$run" "$marks_ok" <<'PY'
import sys, os, re, json
src, dst, inputs, run, marks_ok = sys.argv[1:6]
marks_ok = marks_ok == '1'
env = {}
# Input files may declare marks:   mark NAME /REGEX/ ~FALLBACK_SECONDS
# and use them:                     NAME+<t>[-<t1>]:items     (seconds after the game printed a line matching REGEX; main.cpp resolves it)
# A binary without mark support (no "desktop: input mark" string) gets the entries at FALLBACK+<t> absolute seconds instead.
def expand(v):
    ref, _, rest = v[1:].partition(',')
    shift = 0.0
    if '+' in ref:
        ref, sh = ref.rsplit('+', 1); shift = float(sh)
    path = ref if os.path.isfile(ref) else os.path.join(inputs, ref + '.txt')
    marks, parts = {}, []
    for l in open(path, encoding='utf-8'):
        l = l.strip()
        m = re.match(r'^mark\s+(\w+)\s+/(.*)/\s*(?:~\s*([0-9.]+))?\s*(?:#.*)?$', l)
        if m:
            marks[m[1]] = (m[2], float(m[3]) if m[3] else None)
            continue
        l = l.split('#', 1)[0].strip()
        for e in filter(None, (x.strip() for x in l.split(','))):
            t, _, items = e.partition(':')
            name = None
            mm = re.match(r'^([A-Za-z_]\w*)\+(.*)$', t)
            if mm:
                name, t = mm[1], mm[2]
                if name not in marks: raise SystemExit(f'harness: input {path}: unknown mark {name!r}')
            nums = [float(x) + shift for x in t.split('-')]
            if name and not marks_ok:
                fb = marks[name][1]
                if fb is None: raise SystemExit(f'harness: input {path}: mark {name} has no ~fallback and the binary has no mark support')
                nums = [x + fb for x in nums]; name = None
            tt = '-'.join(f'{x:g}' for x in nums)
            parts.append((name + '+' if name else '') + tt + ':' + items)
    used = {p.split('+')[0] for p in parts if re.match(r'^[A-Za-z_]\w*\+', p)}
    return ','.join(parts + ([rest] if rest else [])), {n: marks[n][0] for n in used}
for line in open(src, encoding='utf-8'):
    line = line.rstrip('\n')
    if '=' not in line: continue
    k, v = line.split('=', 1)
    if k in ('PETARI_INPUT',) and v.startswith('@'):
        v, used = expand(v)
        if used:   # live script: the game re-reads it every 100 ms; the supervisor appends "!NAME" when a mark's line shows up
            open(os.path.join(run, 'input.live'), 'w').write(v + '\n')
            json.dump(used, open(os.path.join(run, 'marks.json'), 'w'))
            env['PETARI_INPUT_FILE'] = os.path.join(run, 'input.live')
            env['HARNESS_MARKS'] = os.path.join(run, 'marks.json')
            env.pop(k, None)
            continue
    env.pop(k, None); env[k] = v
with open(dst, 'w', encoding='utf-8') as f:
    for k, v in env.items(): f.write(f'{k}={v}\n')
PY
rm -f "$run/env.raw"
# autopilot processes (SMG_MP_AUTO=...) open their session only once the console is in play (see supervise.py, mp_core.cpp); HARNESS_AUTO_GO=0 = at boot
if grep -q '^SMG_MP_AUTO=' "$run/env.txt" && ! grep -q '^SMG_MP_AUTO_GO_FILE=' "$run/env.txt" && [ "${HARNESS_AUTO_GO:-1}" != 0 ] && grep -q '^PETARI_INPUT' "$run/env.txt"; then
  echo "SMG_MP_AUTO_GO_FILE=$run/auto.go" >> "$run/env.txt"
fi
gameclock=$(grep -c '^PETARI_GAME_CLOCK=[1-9]' "$run/env.txt")
if [ -z "$timeout" ]; then
  if [ "$gameclock" -gt 0 ]; then timeout=$((seconds * 3 + 120)); else timeout=$((seconds + 150)); fi
fi

# HARNESS_DATA in the process's env (e.g. a different version of the disc, make_alt_data.py) overrides the default
alt_data=$(grep -m1 '^HARNESS_DATA=' "$run/env.txt" | cut -d= -f2-)
[ -n "$alt_data" ] && HARNESS_DATA=$alt_data
# --- binary snapshot, saves, shader cache seed
bin=$(harness_snapshot_bin "$HARNESS_BIN") || exit 2
harness_warm "$bin"
# which commit the binary was built from: HARNESS_BIN_COMMIT, else the line for its sha in $HARNESS_CACHE/bin-commits.tsv (sha12<TAB>commit<TAB>note, appended by whoever builds), else unknown
bsha=$(basename "$bin" | sed 's/^game-//')
bcommit=${HARNESS_BIN_COMMIT:-$(grep -m1 "^$bsha" "$HARNESS_CACHE/bin-commits.tsv" 2>/dev/null | cut -f2)}
echo "${bcommit:-unknown}" > "$run/binary-commit.txt"
[ -d "$HARNESS_DATA/files" ] && [ -d "$HARNESS_DATA/sys" ] || harness_die "no cooked disc at $HARNESS_DATA"
if [ -n "$continue_from" ]; then
  [ -d "$continue_from/saves" ] || harness_die "no saves in $continue_from"
  cp -a "$continue_from/saves/." "$run/saves/" || harness_die "cannot copy saves"
  if [ -d "$continue_from/mp" ]; then cp -a "$continue_from/mp" "$run/mp"; fi
  echo "$continue_from" > "$run/continued-from.txt"
elif [ "$saves" != none ]; then
  [ -d "$saves" ] || harness_die "no saves at $saves"
  cp -a "$saves/." "$run/saves/" || harness_die "cannot copy saves"
fi
# HARNESS_SAVE_SETUP=<ops> in the process's env: edit this run's OWN copy of the save folder (tear, mpbak, mptmp, rm: save_setup.py), before the before-hash
save_setup=$(grep -m1 '^HARNESS_SAVE_SETUP=' "$run/env.txt" | cut -d= -f2-)
if [ -n "$save_setup" ]; then
  "$HARNESS_PY" "$HARNESS_DIR/save_setup.py" "$run/saves" "$save_setup" || harness_die "HARNESS_SAVE_SETUP=$save_setup failed"
  echo "$save_setup" > "$run/save-setup.txt"
fi
"$HARNESS_PY" "$HARNESS_DIR/save_guard.py" hash "$run/saves" "$run/save-before.json" > /dev/null
mkdir -p "$HARNESS_CACHE/mesa"
if [ -s "$HARNESS_CACHE/shaders.bin" ]; then cp "$HARNESS_CACHE/shaders.bin" "$run/shader-cache"; fi

declare -a cmd=("$bin" "$HARNESS_DATA" "$run" "$seconds" "$@")
if [ "$gdb" = 1 ]; then
  cmd=(gdb -q -batch -nx -ex 'set pagination off' -ex 'set print thread-events off' -ex 'handle SIGPIPE nostop noprint pass'
       -ex 'handle SIGUSR1 nostop noprint pass' -ex 'handle SIGUSR2 nostop noprint pass' -ex run
       -ex 'echo \n=== gdb: backtrace ===\n' -ex 'bt 40' -ex 'thread apply all bt 25' -ex 'info registers rip rsp' -ex kill
       --args "${cmd[@]}")
fi
if [ "$xvfb" = 1 ]; then cmd=(xvfb-run -a -s "-screen 0 1280x720x24" "${cmd[@]}"); fi
printf '%q ' "${cmd[@]}" > "$run/cmd.txt"; echo >> "$run/cmd.txt"
echo "$bin" > "$run/binary.txt"
sha256sum "$bin" | cut -d' ' -f1 >> "$run/binary.txt"

touch "$run/.ready"
if [ -n "$waitfile" ]; then
  for _ in $(seq 1 600); do [ -e "$waitfile" ] && break; sleep 0.1; done
fi
nice -n "${HARNESS_NICE:-5}" "$HARNESS_PY" "$HARNESS_DIR/supervise.py" --run-dir "$run" --env-file "$run/env.txt" --timeout "$timeout" \
  --name "$name" -- "${cmd[@]}"

# --- after the run: save guard, crash symbolization, shader cache, summary
"$HARNESS_PY" "$HARNESS_DIR/save_guard.py" hash "$run/saves" "$run/save-after.json" > /dev/null
"$HARNESS_PY" "$HARNESS_DIR/save_guard.py" compare "$run/save-before.json" "$run/save-after.json" "$run/save-guard.json" > "$run/save-guard.txt"
guard_rc=$?
if [ "$saves" = none ]; then guard_rc=0; fi
# the multiplayer save root (desktop: <run-dir>/mp/saves, MpSaveGame.cpp): listed, never judged here
"$HARNESS_PY" "$HARNESS_DIR/save_guard.py" hash "$run/mp" "$run/mp-saves.json" > /dev/null
crashline=$(grep -m1 -E 'desktop crash signal=|received signal SIG(SEGV|BUS|ILL|FPE|ABRT|TRAP)' "$run/console.log" 2>/dev/null)
if [ -n "$crashline" ] || grep -q '=== gdb: backtrace ===' "$run/console.log" 2>/dev/null; then
  {
    echo "$crashline"
    # every address of the game image in the crash line (non-PIE at 0x98000000)
    for a in $(echo "$crashline" | grep -oE '(pc|lr|bt)=0x[0-9a-f]+' | cut -d= -f2); do
      printf '%s  ' "$a"; llvm-addr2line -f -C -i -e "$bin" "$a" | paste -sd' '
    done
    # the faulting instruction (address=0 with an aligned SSE store such as movaps is a #GP: a misaligned pointer, not null)
    pc=$(echo "$crashline" | grep -oE 'pc=0x[0-9a-f]+' | cut -d= -f2)
    if [ -n "$pc" ]; then
      objdump -d --no-show-raw-insn --start-address=$((pc - 24)) --stop-address=$((pc + 8)) "$bin" 2>/dev/null | grep -E '^ +[0-9a-f]+:' | sed "s/^ *$(printf '%x' $((pc)))/=>&/"
    fi
    sed -n '/=== gdb: backtrace ===/,$p' "$run/console.log" | cut -d' ' -f2- | head -400
  } > "$run/crash.txt"
fi
# frame pacing (PETARI_FRAMETIMES, a build with the main.cpp recorder): p50/p95/p99, hitches
if [ -s "$run/frametimes.txt" ]; then
  "$HARNESS_PY" "$HARNESS_DIR/frametimes.py" "$run" --json "$run/frametimes.json" > "$run/frametimes-summary.txt"
fi
# a stall (PETARI_STALL_S) or PETARI_THREADS thread dump: symbolized into stall.txt
if grep -q ' *bt: ' "$run/console.log" 2>/dev/null; then
  "$HARNESS_PY" "$HARNESS_DIR/symbolize_stall.py" "$run" -o "$run/stall.txt"
fi
if [ -s "$run/shader-cache" ]; then
  (
    flock 9
    if [ ! -s "$HARNESS_CACHE/shaders.bin" ] || [ "$(stat -c%s "$run/shader-cache")" -gt "$(stat -c%s "$HARNESS_CACHE/shaders.bin")" ]; then
      cp "$run/shader-cache" "$HARNESS_CACHE/shaders.bin.tmp.$$" && mv "$HARNESS_CACHE/shaders.bin.tmp.$$" "$HARNESS_CACHE/shaders.bin"
    fi
  ) 9> "$HARNESS_CACHE/shaders.lock"
  rm -f "$run/shader-cache"
fi
"$HARNESS_PY" - "$run" "$guard_rc" "$HARNESS_DIR" <<'PY'
import json, os, sys
run, guard_rc = sys.argv[1], int(sys.argv[2])
r = json.load(open(os.path.join(run, 'result.json')))
g = json.load(open(os.path.join(run, 'save-guard.json')))
shots = sorted(f for f in os.listdir(os.path.join(run, 'shots')) if f.endswith('.png'))
log = os.path.join(run, 'game.log')
r.update({'run_dir': run, 'shots': len(shots), 'stalled': os.path.exists(os.path.join(run, 'stall.txt')), 'save_guard_unchanged': g['unchanged'], 'save_guard_rc': guard_rc, 'save_folder_changed': g.get('folder_changed'),
          'game_log_lines': sum(1 for _ in open(log, errors='replace')) if os.path.exists(log) else 0,
          'crash_report': os.path.exists(os.path.join(run, 'crash.txt'))})
ft = os.path.join(run, 'frametimes.json')
if os.path.exists(ft):
    f = json.load(open(ft)); r['frametimes'] = {k: f[k] for k in ('frames', 'fps', 'p50_ms', 'p95_ms', 'p99_ms', 'max_ms', 'hitches_over_50_ms')}
sys.path.insert(0, sys.argv[3])
import memsummary
mem = memsummary.summarize(run)
r['memory'] = mem
r['mem_line'] = memsummary.line(mem)
ok = (r['exit_code'] == 0 or r.get('killed_by_harness')) and not r['crash'] and not r['timed_out'] and guard_rc == 0 and not r.get('memory_ceiling_exceeded')
r['ok'] = ok
json.dump(r, open(os.path.join(run, 'summary.json'), 'w'), indent=1)
print(f"run_one {r['name']} {r['mem_line']}")
print(f"run_one {r['name']}: {'OK' if ok else 'FAIL'} exit={r['exit_code']} signal={r['signal']} timeout={r['timed_out']} "
      f"wall={r['wall_s']}s cpu={r['cpu_s']}s peakRSS={r['peak_hwm_kb'] // 1024}MiB shots={len(shots)} "
      f"{' frames p50/p95/p99=%s/%s/%s ms hitches=%s' % (r['frametimes']['p50_ms'], r['frametimes']['p95_ms'], r['frametimes']['p99_ms'], r['frametimes']['hitches_over_50_ms']) if r.get('frametimes') else ''} "
      f"save={'unchanged' if g['unchanged'] else 'CHANGED/MISSING'}{' STALL (stall.txt)' if r['stalled'] else ''}{' CRASH: ' + r['crash'] if r['crash'] else ''}  [{run}]")
sys.exit(0 if ok else 1)
PY
}
main "$@"; exit $?

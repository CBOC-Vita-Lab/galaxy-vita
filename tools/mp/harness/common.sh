# Shared defaults of the MP test harness (sourced by run_one.sh / run_pair.sh).
# Every value can be overridden from the environment.
HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HARNESS_BIN=${HARNESS_BIN:-/home/claude/g3/desk/build/game}          # the desktop game (platform/desktop/main.cpp)
HARNESS_DATA=${HARNESS_DATA:-/data/cooked}                            # the owner's cooked disc (files/, sys/); never copied anywhere
HARNESS_SAVES0=${HARNESS_SAVES0:-/data/saves0/saves}                  # the test save; copied into each run, never written
HARNESS_OUT=${HARNESS_OUT:-/home/claude/g3/hruns}                     # run folders (outside the repo)
HARNESS_CACHE=${HARNESS_CACHE:-$HARNESS_OUT/.cache}                   # binary snapshots + the shared shader cache
HARNESS_PY=${HARNESS_PY:-python3}

# The headless environment every game process gets (a scenario / -e can override each key).
#  EGL_PLATFORM=surfaceless + Mesa llvmpipe: no X server, no GPU. headless_render.cpp takes eglGetDisplay(EGL_DEFAULT_DISPLAY),
#  which Mesa maps to the surfaceless platform with this variable; xvfb-run is not needed (HARNESS_XVFB=1 forces it anyway).
#  LP_NUM_THREADS=1: llvmpipe would start one raster thread per core in each process; two games on 2 cores fight less with 1.
#  SDL_AUDIODRIVER=dummy: SDL's dummy device still pulls audio at the real rate, so the game's audio clock behaves.
#  GLIBC_TUNABLES: the allocation policy of tools/run_native.sh (os_thread.cpp's heap statistics assume it).
harness_base_env() {
  cat <<EOF
EGL_PLATFORM=surfaceless
LIBGL_ALWAYS_SOFTWARE=1
GALLIUM_DRIVER=llvmpipe
LP_NUM_THREADS=${HARNESS_LP_THREADS:-1}
MESA_SHADER_CACHE_DIR=$HARNESS_CACHE/mesa
SDL_AUDIODRIVER=dummy
GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0
PETARI_SHOT_MS=5000
PETARI_STALL_S=30
PETARI_POSLOG=1
PETARI_LOG_FRAMES=1
EOF
  # A test build carries the PRODUCTION service address (cmake/MpBuildDefs.cmake: https://galaxy.psvita.dev): a scenario must never reach the live service.
  # SMG_ONLINE_URL=none switches online off (mp_service.cpp: an invalid value = no service). A scenario that needs the local fake server sets its own
  # SMG_ONLINE_URL (env: / -e / -P win over this default), and so does a wrapper script that EXPORTS one (supervise.py starts the game with the caller's
  # environment, the env file on top): the default is emitted only when the caller's environment has none. HARNESS_LIVE_SERVICE=1 keeps the build's own address (never in a sweep).
  if [ "${HARNESS_LIVE_SERVICE:-0}" != 1 ] && [ -z "${SMG_ONLINE_URL:-}" ]; then printf 'SMG_ONLINE_URL=none\n'; fi
  # PETARI_LOG_FRAMES=1: every game.log line carries "@f<game frame>" (hlog.py strips it): rules in game time (gs / f) are exact. A binary
  # built before that switch ignores it (the judge then interpolates from the coarser frame points).
  # HARNESS_NODRAW=1: GXV_SKIP_ALL=1, the renderer rasterises no frame except a captured one (screenshots stay right; PETARI_SHOT_MS controls
  # how many are drawn). Simulation, networking and the GX front end still run. Off by default; a binary without the switch ignores it.
  if [ "${HARNESS_NODRAW:-0}" = 1 ]; then printf 'GXV_SKIP_ALL=1\n'; fi
  # Vita button pictures need BOTH of these (the picture swap lives in the Vita renderer, vgx; the default desktop renderer ignores it).
  # HARNESS_HANDHELD=0 turns the pair off. HARNESS_SHOT_SIZE=960x544 (the console's screen) makes the captures that size (GXV_CAP_PRESENT).
  if [ "${HARNESS_HANDHELD:-1}" != 0 ]; then printf 'PETARI_HANDHELD_UI=1\nPETARI_BACKEND=vgx\n'; fi
  # VITA LIMITS (HARNESS_VITA_LIMITS=1): the console's memory banks and the memory-related defaults of vita/vgx-game/entry.cpp. See README (emu).
  #  PETARI_VITA_LIMITS=1 (platform/src/os/os_mem.cpp): MEM1 24 MiB + MEM2 64 MiB instead of 128 + 128: the game's JKR heaps run dry where the
  #  console's do (same OSPanic / "heap error:" path). A binary built before that hook ignores it; the run summary says "REQUESTED BUT THE BINARY IGNORED IT".
  #  The GXV_* lines are entry.cpp's setenv(.., 0) defaults for memory (host `new` outside the C heap, scene-drop of renderer objects).
  if [ "${HARNESS_VITA_LIMITS:-0}" = 1 ]; then
    printf 'PETARI_VITA_LIMITS=1\nGXV_HEAP_BIG=1\nGXV_HEAP_BIG_KIB=128\nGXV_HEAP_BIG_FLOOR_MIB=16\nGXV_HEAP_BIG_CACHE_MIB=8\nGXV_HEAP_BIG_LAST_MIB=4\n'
    printf 'GXV_COPY_DROP=1\nGXV_SL_SCENEDROP=2\nGXV_TEX_SCENEDROP=3\n'
  fi
  # Process ceilings (any run): HARNESS_AS_MIB=<n> = RLIMIT_AS, HARNESS_RSS_CEILING_MIB=<n> = kill past this resident size (supervise.py)
  [ -n "${HARNESS_AS_MIB:-}" ] && printf 'HARNESS_AS_MIB=%s\n' "$HARNESS_AS_MIB"
  [ -n "${HARNESS_RSS_CEILING_MIB:-}" ] && printf 'HARNESS_RSS_CEILING_MIB=%s\n' "$HARNESS_RSS_CEILING_MIB"
  if [ -n "${HARNESS_SHOT_SIZE:-}" ]; then printf 'GXV_CAP_PRESENT=%s\n' "$HARNESS_SHOT_SIZE"; fi
}

harness_die() { echo "harness: $*" >&2; exit 2; }

# Snapshot the game binary (the coordinator may relink it while a run is going): $HARNESS_CACHE/bin/game-<sha12>
harness_snapshot_bin() {
  local bin=$1 sum dst
  [ -x "$bin" ] || harness_die "no game binary at $bin (HARNESS_BIN)"
  mkdir -p "$HARNESS_CACHE/bin"
  sum=$(sha256sum "$bin" | cut -c1-12)
  dst=$HARNESS_CACHE/bin/game-$sum
  if [ ! -x "$dst" ]; then cp "$bin" "$dst.tmp.$$" && mv "$dst.tmp.$$" "$dst"; fi
  echo "$dst"
}

# WARM-UP: the first game run after a machine restart finds the disc data and the binary cold in the page cache (one first boot took 30 s to
# initialise instead of 0.4 s). Once per boot (stamp /dev/shm/harness-warm.stamp: /dev/shm is empty after a restart) the data and the binary
# are read once, under a lock, before the run starts; HARNESS_WARM=0 skips it. Prints one line saying what it did.
harness_warm() {
  [ "${HARNESS_WARM:-1}" = 0 ] && return 0
  local stamp=/dev/shm/harness-warm.stamp
  [ -e "$stamp" ] && return 0
  (
    flock 9
    [ -e "$stamp" ] && exit 0
    local t0=$(date +%s)
    find "$HARNESS_DATA" -type f -print0 2>/dev/null | xargs -0 -r cat > /dev/null 2>&1
    [ -n "${1:-}" ] && cat "$1" > /dev/null 2>&1
    echo "warmed $(date +%F\ %T) in $(( $(date +%s) - t0 )) s (data $HARNESS_DATA)" > "$stamp"
    echo "harness: cold machine (no $stamp): read the game data once, $(( $(date +%s) - t0 )) s" >&2
  ) 9> /dev/shm/harness-warm.lock
}

# PORT LEASES (emu): a block of SIZE consecutive loopback UDP ports, leased for the life of the calling script.
#  One lease file per port under $HARNESS_PORT_DIR (default /home/claude/g3/gamerun.d/ports: the same folder for every agent, whatever
#  HARNESS_OUT it uses; the old locks lived under $HARNESS_CACHE, which differs per agent, so two agents could take the same block).
#  Each port's file is flock'ed (non-blocking) on a file descriptor that stays open until the script exits; the kernel drops the lock when
#  the process dies, however it dies (kill -9 included), so a stale lease cannot exist. The file also holds "pid label start" for
#  `harness_port_status`; a record whose pid is dead is shown as stale and ignored. A candidate block is also bind-probed (some other
#  program may use a port without a lease). The caller's children inherit the descriptors: an orphaned game keeps its block until it ends.
#  harness_port_lease SIZE [FIRST] -> sets LEASE_BASE (and LEASE_FDS). Returns 1 when nothing is free in FIRST..41999.
HARNESS_PORT_DIR=${HARNESS_PORT_DIR:-/home/claude/g3/gamerun.d/ports}
LEASE_FDS=()
LEASE_BASE=
harness_port_release() { local fd; for fd in "${LEASE_FDS[@]}"; do exec {fd}>&-; done; LEASE_FDS=(); LEASE_BASE=; }
harness_port_lease() {
  local size=$1 first=${2:-${HARNESS_PORT_FIRST:-41110}} only=${3:-} step b p fd ok
  step=$(( (size + 9) / 10 * 10 )); [ "$step" -lt 10 ] && step=10
  mkdir -p "$HARNESS_PORT_DIR" 2>/dev/null || return 1
  chmod 1777 "$HARNESS_PORT_DIR" 2>/dev/null
  for ((b = first; b + size <= ${HARNESS_PORT_LAST:-42000}; b += step)); do
    LEASE_FDS=(); ok=1
    for ((p = b; p < b + size; p++)); do
      { exec {fd}>>"$HARNESS_PORT_DIR/$p.lease"; } 2>/dev/null || { ok=0; break; }
      LEASE_FDS+=("$fd")
      if ! flock -n "$fd"; then ok=0; break; fi
    done
    if [ "$ok" = 1 ] && ! "$HARNESS_PY" - "$b" "$size" <<'PY'
import socket, sys
b, n = int(sys.argv[1]), int(sys.argv[2])
try:
    for p in range(b, b + n):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', p)); s.close()
except OSError:
    sys.exit(1)
PY
    then ok=0; fi
    if [ "$ok" = 1 ]; then
      LEASE_BASE=$b
      # who holds it (for harness_port_status); written through the first descriptor after truncation
      printf '%s %s %s size=%s\n' "$$" "${HARNESS_LEASE_LABEL:-run}" "$(date +%s)" "$size" > "/proc/self/fd/${LEASE_FDS[0]}" 2>/dev/null
      return 0
    fi
    harness_port_release
    [ -n "$only" ] && return 1
  done
  return 1
}
harness_port_lease_at() { harness_port_lease "$2" "$1" only; }
harness_port_status() {  # lists the held leases: port pid label age, and stale records
  local f p rec pid
  for f in "$HARNESS_PORT_DIR"/*.lease; do
    [ -e "$f" ] || continue
    p=$(basename "$f" .lease); rec=$(cat "$f" 2>/dev/null)
    if flock -n 9 9>>"$f"; then [ -n "$rec" ] && echo "$p free (stale record: $rec)"; else echo "$p held: $rec"; fi
  done
}
# harness_start_on_match <regex> <file>: does a line of <file> match <regex>? The dialect is the one every scenario regex is written in
# (Python-like: \d \w \S, \[ ...), so PCRE; plain `grep -E` reads \d as the letter d and the late joiner of a HARNESS_START_ON never starts.
harness_start_on_match() {
  [ -r "$2" ] || return 1   # joinfix: the awaited console has not started yet (no console.log): not a match yet, NOT a bad regex (grep exit 2 killed the waiter)
  command grep -qP -- "$1" "$2" 2>/dev/null
  local rc=$?
  [ $rc -le 1 ] || { echo "harness: HARNESS_START_ON regex is not valid PCRE (grep exit $rc): $1" >&2; exit 2; }   # a bad regex must not wait for the host to end
  return $rc
}

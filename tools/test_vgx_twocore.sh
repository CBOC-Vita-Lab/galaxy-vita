#!/usr/bin/env bash
# GXV_FE_TWOCORE (platform/vgx/src/vgx_twocore.h): the two-core front end on the host, with the J3D-shaped scene of tests/vgx_fe_work.cpp
# through the real front end and its second instance (vgx_twocore_b*.cpp). Passes when
#   - for every workload and setting below, GXV_FE_TWOCORE=1 gives the command buffers (commands, records with their alpha bounds, uniform /
#     index / vertex arenas, uploads) of GXV_FE_TWOCORE=0 frame for frame, in several runs (the segments land elsewhere in each: thread timing);
#   - the second worker's segments are taken (at least MIN_TAKEN per picture on the plain scene, with the console's settings, which now
#     include GXV_SHADOW_RECT=1);
#   - GXV_FE_TWOCORE=2 (verify) reports no differing state at segment starts and ends, and gives =0's buffers;
#   - twocore24, the console's threads (FEW_GAME=1: a game thread publishing in pieces and waiting WITHOUT a bound for a view job of a worker,
#     a loading thread waiting for each finish, scene drops with GXV_TEX_SCENEDROP=3 / GXV_SL_SCENEDROP=2, the stream moved to another
#     segment in the middle of a picture): =0, =1 and =2 identical, nothing hangs;
#   - twocore24, starvation: B sleeping at random at every hand-over (GXV_FE_TWOCORE_INJECT=3) with the split's jitter; with SCHED_FIFO (when
#     the machine allows it) B and the worker on one CPU in the console's priority order, B at 0xA0 and at the old 0x93: identical, no hang;
#   - twocore24, the fall-back: a B that never finishes a claimed segment (INJECT=1, for good and for 40 ms), and a B that holds the shared
#     lock 30 ms (INJECT=2): the pictures identical to =0 and the switch turns itself off ("turns itself off" in the log);
#   - the fixture built against the base tree (REF_ROOT, optional) gives =0's buffers (the switch off changes nothing);
#   - an AddressSanitizer + UndefinedBehaviorSanitizer build and a ThreadSanitizer build of =1 report nothing;
#   - twocore25, the console's stream shape (FEW_CONSOLE=1: J3D's small differed lists before most shapes, small shapes drawn inline, lists
#     the cache bypasses, particles) with the settings of build c49 (no shadow rectangle, no character AA: GXV_DIRECT_J3D's deferred key
#     copies stay deferred): =2 reports 0 differing at starts and ends with at least 20 ends checked, =1 and =2 give =0's buffers, B made
#     at least MIN_MADE % of the bytes (the tree before twocore25 reported "ends n (most differing)" here, as the console did);
#   - twocore25, the rules for play: pictures not in play (GXV_FE_TWOCORE_TEST_QUIET) and while settling are not given to B, B rests when it
#     makes too little (GXV_FE_TWOCORE_MINSHARE), identical buffers either way.
# Every binary is built with std::this_thread::yield() empty (FEW_VITA_YIELD, -Wl,--wrap=sched_yield), as the console's C++ library has it.
#   tools/test_vgx_twocore.sh [frames] [seeds] [runs]     (OUT=<dir>, default /tmp/vgx_twocore; CXX, TSAN_CXX; REPRO_ROOT=<tree before
#   twocore24>: also shows that tree's freeze in the console's threads, for the record; REPRO25_BIN=<the fixture built from the front end before
#   twocore25>: that tree's verify failure on the console's stream shape, for the record; neither is a pass condition)
# A build that fails stops the script with exit code 2 and the compiler's last lines (it no longer passes through a pipe as success).
set -uo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
export FEW_HASHAB=1 # (the fixture hashes the records' alpha bounds only when asked: this test needs them compared)
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-40}
seeds=${2:-"1 2"}
runs=${3:-2}
out=${OUT:-$MP_TMP/vgx_twocore}
mkdir -p "$out"
: > "$out/summary.txt"
cxx=${CXX:-g++}
S=platform/vgx/src
fe=($S/vgx_state.cpp $S/vgx_draw.cpp $S/vgx_fesplit.cpp $S/vgx_dlcache.cpp $S/vgx_guestmem.cpp $S/vgx_bpsync.cpp
    $S/vgx_shadergen.cpp $S/vgx_texcache.cpp $S/vgx_texcomp.cpp $S/vgx_texdecode.cpp $S/vgx_stats.cpp $S/vgx_backend_null.cpp)
two=($S/vgx_twocore_b.cpp $S/vgx_twocore_b_state.cpp $S/vgx_twocore_b_draw.cpp $S/vgx_twocore_b_dlcache.cpp $S/vgx_twocore_b_fesplit.cpp
     $S/vgx_twocore_b_bpsync.cpp $S/vgx_twocore_b_shadergen.cpp $S/vgx_twocore_b_stats.cpp)
srcs=(tests/vgx_fe_work.cpp "${fe[@]}" "${two[@]}")
cd "$root"
common=(-std=c++17 -g -I$S -Iplatform/vgx/third_party -pthread -DFEW_VITA_YIELD -Wl,--wrap=sched_yield)
build() { # name, compiler, flags... (the sources: srcs)
  local name=$1 c=$2; shift 2
  if ! "$c" "${common[@]}" "$@" "${srcs[@]}" -o "$out/$name" > "$out/build-$name.log" 2>&1 || [ ! -x "$out/$name" ]; then
    echo "FAIL: the build of $name failed (exit code 2):"; grep -E "error|undefined reference" "$out/build-$name.log" | head -20
    [ -s "$out/build-$name.log" ] && tail -5 "$out/build-$name.log"
    exit 2
  fi
}
if [ -z "${NOBUILD:-}" ]; then
  rm -f "$out/few" "$out/few-asan" "$out/few-tsan" "$out/few-ref"
  build few "$cxx" -O2
  build few-asan "$cxx" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
  build few-tsan "${TSAN_CXX:-g++}" -O1 -fsanitize=thread
  if [ -n "${REF_ROOT:-}" ]; then
    refsrcs=(tests/vgx_fe_work.cpp)
    for f in "${fe[@]}"; do refsrcs+=("$REF_ROOT/$f"); done
    if ! $cxx -std=c++17 -g -I"$REF_ROOT/$S" -Iplatform/vgx/third_party -pthread -O2 "${refsrcs[@]}" -o "$out/few-ref" > "$out/build-ref.log" 2>&1; then
      echo "FAIL: the build against REF_ROOT failed (exit code 2):"; grep -E "error|undefined reference" "$out/build-ref.log" | head; exit 2
    fi
  fi
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# (the console's front-end settings, vita/vgx-game/entry.cpp, as far as the fixture has them; the split is on as on the console)
vita="GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1
      GXV_STATICCULL_PAL=1 GXV_DYNCULL=1 GXV_DLWELD=1 GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24
      GXV_FE_PARALLEL=1"
# (twocore24: the console's defaults at a67d200 on top: the shadow rectangle (ShadowFillOn), canonical TEV inputs 2, retained and direct J3D
# shapes, rigid merge, the scene drops; FEW_ALPHA=1 makes a third of the materials write alpha, so that the alpha bounds are worked out)
console="$vita GXV_SHADOW_RECT=1 GXV_TEVIN_CANON=2 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_TEX_SCENEDROP=3 GXV_SL_SCENEDROP=2
         FEW_RETAINED=1 FEW_ALPHA=1"
# (twocore25: build c49's settings, dce2f58, as the console's tw6 / tw7 runs had them: no shadow rectangle, no character AA)
c49="$vita GXV_CHARAA_AUTO=0 GXV_SHADOW_RECT=0 GXV_TEVIN_CANON=2 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_TEX_SCENEDROP=3
     GXV_SL_SCENEDROP=2 FEW_RETAINED=1 FEW_ALPHA=1"
cshape="FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150"
# (the use rule's rests and the settling are tested on their own, section 8; everywhere else B gets every picture)
export GXV_FE_TWOCORE_MINSHARE=0
# (twocore25: the next picture waits for B at most GXV_FE_TWOCORE_START_US (50 us on the console); the host's pictures are ~0.5 ms and B shares
# two CPUs with the player and the finishing thread, so B often has not let go by then and the picture goes without it. The engagement checks
# (sections 2 / 2b) wait up to 2 ms, as twocore24's picture end did; every other run keeps the console's value)
eng="GXV_FE_TWOCORE_START_US=2000"
fail=0
pass_runs=0
tmo=${TMO:-120}
# one run: name env... -> $out/<name>.txt / .err; a hang (timeout or the fixture's watch) or a crash is a failure
run() {
  local name=$1 bin=$2; shift 2
  env "$@" FEW_GAME_WATCH=${WATCH:-20} timeout "$tmo" "$out/$bin" "${FR:-$frames}" "$seed" > "$out/$name.txt" 2> "$out/$name.err"
  local rc=$?
  if [ $rc = 124 ] || grep -q "fixture: HANG" "$out/$name.err"; then echo "FAIL seed $seed: HANG ($name): $*" | cut -c1-300; fail=1; return 1; fi
  if [ $rc != 0 ]; then echo "FAIL seed $seed: exit code $rc ($name): $*" | cut -c1-300; grep -m3 "fixture:\|ERROR\|runtime error" "$out/$name.err"; fail=1; return 1; fi
  return 0
}
same() { # a b label
  if cmp -s "$out/$1.txt" "$out/$2.txt"; then pass_runs=$((pass_runs + 1)); return 0; fi
  echo "FAIL seed $seed: $3: the command buffers differ"; diff "$out/$1.txt" "$out/$2.txt" | head -3; fail=1; return 1
}
taken() { sed -n 's/.*per picture: segments [0-9.]* taken \([0-9.]*\).*/\1/p' "$out/$1.err" | tail -1; }
for seed in $seeds; do
  # ---- 1. the workloads and setting groups (the plain loop: a whole picture handed over at once)
  for wl in "FEW_ODD=0" "FEW_ODD=1" "FEW_ODD=0 FEW_RETAINED=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1" "FEW_ODD=0 FEW_JPA=150 FEW_FX=60" \
            "FEW_ODD=0 FEW_FUR=10 FEW_PLANTS=40" "FEW_ODD=1 FEW_BIG=20 FEW_RETAINED=1 GXV_RETAINED=1" "FEW_ODD=0 FEW_JPA=120 FEW_FXMERGE=1 GXV_FXMERGE=1" \
            "FEW_ODD=0 FEW_TRASH=256" "FEW_ODD=0 FEW_ALPHA=1" "$cshape"; do
    for around in "$vita" "GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=0" "$vita GXV_ABSIDX=0" "$vita GXV_FECUT_KEYID=0 GXV_FE_PARALLEL=0" "$vita GXV_SPLIT_JITTER=1" \
                  "$console" "$c49"; do
      run off few $around $wl GXV_FE_TWOCORE=0 || continue
      for r in $(seq "$runs"); do
        run on few $around $wl GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off on "GXV_FE_TWOCORE=1, $wl | $(echo $around | cut -c1-60)"
      done
      echo "    $wl | $(echo $around | cut -c1-40): $(grep 'fetwo: GXV' "$out/on.err" | head -1 | cut -c1-200)" >> "$out/summary.txt"
    done
  done
  # ---- 2. engagement and verify, the plain scene with the console's settings
  run off few $console FEW_ODD=0 GXV_FE_TWOCORE=0
  run on few $console FEW_ODD=0 GXV_FE_TWOCORE=1 $eng GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off on "the console's settings"
  t=$(taken on)
  if ! awk -v t="${t:-0}" -v m="${MIN_TAKEN:-0.5}" 'BEGIN { exit !(t >= m) }'; then echo "FAIL seed $seed: segments taken per picture ${t:-?} < ${MIN_TAKEN:-0.5} (the console's settings)"; fail=1; fi
  if grep "without B [1-9]" "$out/on.err" | grep -v "without B [0-9]* (B still in an earlier picture [0-9]*)" | grep -q .; then echo "FAIL seed $seed: pictures without B with the console's settings: $(grep -o 'without B [^;]*' "$out/on.err" | head -1)"; fail=1; fi
  ab=$(grep -o "records with alpha bounds [0-9]*" "$out/on.err" | grep -o "[0-9]*$")
  if [ "${ab:-0}" -lt 1 ]; then echo "FAIL seed $seed: no record with alpha bounds: the shadow-fill path was not exercised"; fail=1; fi
  echo "seed $seed: the console's settings: segments taken per picture $t; records with alpha bounds $ab" >> "$out/summary.txt"
  run ver few $console FEW_ODD=0 GXV_FE_TWOCORE=2 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off ver "GXV_FE_TWOCORE=2"
  if grep -q "fetwo: MISMATCH" "$out/ver.err"; then echo "FAIL seed $seed: verify: $(grep -m1 'fetwo: MISMATCH' "$out/ver.err")"; fail=1; fi
  verline=$(grep "fetwo: GXV" "$out/ver.err" | head -1 | grep -o "verify: [^;]*" || true)
  set -- $(echo "$verline" | sed 's/[^0-9 ]/ /g')
  if [ "${1:-0}" -lt 5 ] || [ "${2:-1}" != 0 ] || [ "${3:-0}" -lt 1 ] || [ "${4:-1}" != 0 ]; then echo "FAIL seed $seed: verify counts: $verline"; fail=1; fi
  # ---- 2b. (twocore25) the console's stream shape with c49's settings: verify clean, =1 and =2 exact, B makes a share of it
  run off few $c49 $cshape GXV_FE_TWOCORE=0
  for r in $(seq "$runs"); do
    run ver few $c49 $cshape GXV_FE_TWOCORE=2 $eng GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off ver "GXV_FE_TWOCORE=2, the console's stream shape"
    if grep -q "fetwo: MISMATCH" "$out/ver.err"; then echo "FAIL seed $seed: verify, the console's stream shape: $(grep -m1 'fetwo: MISMATCH' "$out/ver.err")"; fail=1; fi
    verline=$(grep "fetwo: GXV" "$out/ver.err" | head -1 | grep -o "verify: [^;]*" || true)
    set -- $(echo "$verline" | sed 's/[^0-9 ]/ /g')
    if [ "${1:-0}" -lt 20 ] || [ "${2:-1}" != 0 ] || [ "${3:-0}" -lt 20 ] || [ "${4:-1}" != 0 ]; then echo "FAIL seed $seed: verify counts, the console's stream shape: $verline"; fail=1; fi
    made=$(grep -o "B made [0-9.]* %" "$out/ver.err" | head -1 | grep -o "[0-9.]*")
    if ! awk -v t="${made:-0}" -v m="${MIN_MADE:-1}" 'BEGIN { exit !(t >= m) }'; then echo "FAIL seed $seed: B made ${made:-?} % < ${MIN_MADE:-1} % of the console-shaped stream (=2)"; fail=1; fi
    run on few $c49 $cshape GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off on "GXV_FE_TWOCORE=1, the console's stream shape"
    run on few $c49 $cshape GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_INJECT=4 GXV_FE_TWOCORE_SLOTS=32 GXV_FE_TWOCORE_LEAD=1 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 \
      && same off on "GXV_FE_TWOCORE=1, the console's stream shape, B first (every segment it can make)"
    run on few $c49 $cshape GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_TINY=0 GXV_FE_TWOCORE_SLOTS=4 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off on "GXV_FE_TWOCORE=1 TINY=0 SLOTS=4, the console's stream shape"
  done
  echo "seed $seed: the console's stream shape (c49): $verline; B made $made % of the bytes (=2)" >> "$out/summary.txt"
  # ---- 3. the console's threads (FEW_GAME=1): game thread in pieces, view-job worker, loading thread, scene drops, segment moves
  for wl in "FEW_ODD=0" "FEW_ODD=0 FEW_SCENE_DROP=7" "FEW_ODD=0 FEW_SEGMOVE=3" "FEW_ODD=1 FEW_JPA=80 FEW_SCENE_DROP=5 FEW_SEGMOVE=4" \
            "FEW_ODD=0 FEW_FUR=10 FEW_PLANTS=40 FEW_SCENE_DROP=9" "$cshape FEW_SCENE_DROP=7 FEW_SEGMOVE=4"; do
    run goff few $console $wl FEW_GAME=1 GXV_FE_TWOCORE=0 || continue
    for m in 1 2; do
      for r in $(seq "$runs"); do
        run gon few $console $wl FEW_GAME=1 GXV_FE_TWOCORE=$m GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same goff gon "FEW_GAME=1 GXV_FE_TWOCORE=$m, $wl"
      done
      if [ $m = 2 ] && grep -q "fetwo: MISMATCH" "$out/gon.err"; then echo "FAIL seed $seed: verify in FEW_GAME: $wl"; fail=1; fi
    done
    echo "    FEW_GAME $wl: taken per picture $(taken gon); $(grep -o 'cancelled in the middle [0-9]*' "$out/gon.err" | head -1)" >> "$out/summary.txt"
  done
  # ---- 4. starvation: random sleeps at every hand-over, the split's jitter; real-time priorities in the console's order
  run goff few $console FEW_ODD=0 FEW_GAME=1 FEW_SCENE_DROP=7 FEW_SEGMOVE=5 GXV_FE_TWOCORE=0
  run gon few $console FEW_ODD=0 FEW_GAME=1 FEW_SCENE_DROP=7 FEW_SEGMOVE=5 GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_INJECT=3 GXV_SPLIT_JITTER=1 FEW_LOG=1 \
    && same goff gon "starved B (GXV_FE_TWOCORE_INJECT=3, split jitter)"
  if chrt -f 1 true 2> /dev/null && [ "$(nproc)" -ge 2 ]; then
    run goff few $console FEW_ODD=0 FEW_GAME=1 FEW_SEG_GAP=1 FEW_GAME_RT=1 GXV_FE_TWOCORE=0
    for prio in 0xA0 0x93; do
      run gon few $console FEW_ODD=0 FEW_GAME=1 FEW_SEG_GAP=1 FEW_GAME_RT=1 GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_PRIO=$prio FEW_LOG=1 GXV_FE_TWOCORE_LOG=100000 \
        && same goff gon "SCHED_FIFO, B at $prio"
      echo "    SCHED_FIFO B $prio: taken per picture $(taken gon)" >> "$out/summary.txt"
    done
  else
    echo "seed $seed: (SCHED_FIFO not available here: the real-time placement runs are left out)" | tee -a "$out/summary.txt"
  fi
  # ---- 5. the fall-back: B that never finishes, B that holds the lock; identical pictures, the switch goes off by itself
  # (at least 60 pictures; GXV_FE_TWOCORE_TRIP=2 hard timeouts, or 30 pictures in a row that B has not let go of, turn it off)
  fb=$(( frames > 60 ? frames : 60 ))
  # (the lock case with scene drops: the player re-creates textures under the lock while B holds it)
  FR=$fb run goff few $console FEW_ODD=0 FEW_GAME=1 FEW_SCENE_DROP=3 GXV_FE_TWOCORE=0
  # (twocore25: a B that stalls 40 ms and recovers no longer turns the switch off: the pictures it misses go without it; it must not hang)
  for inj in "GXV_FE_TWOCORE_INJECT=1 GXV_FE_TWOCORE_INJECT_MS=0" "GXV_FE_TWOCORE_INJECT=1 GXV_FE_TWOCORE_INJECT_MS=40" \
             "GXV_FE_TWOCORE_INJECT=2 GXV_FE_TWOCORE_INJECT_MS=30"; do
    FR=$fb run gon few $console FEW_ODD=0 FEW_GAME=1 FEW_SCENE_DROP=3 GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_TRIP=2 $inj FEW_LOG=1 GXV_FE_TWOCORE_LOG=100000 && same goff gon "fall-back, $inj"
    case "$inj" in *INJECT_MS=40*) ;; *) if ! grep -q "turns itself off" "$out/gon.err"; then echo "FAIL seed $seed: $inj: the switch did not turn itself off"; fail=1; fi ;; esac
    echo "    $inj: $(grep -o 'hard timeout: [^(]*' "$out/gon.err" | head -1); $(grep -o 'turns itself off for the rest of the session: [^(]*' "$out/gon.err" | head -1)" >> "$out/summary.txt"
  done
  # ---- 8. (twocore25) the rules for play: not in play (the first 15 pictures), settling (5), rests when B makes too little; identical
  run off few $c49 $cshape GXV_FE_TWOCORE=0
  run on few $c49 $cshape GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_TEST_QUIET=15 GXV_FE_TWOCORE_SETTLE=5 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 && same off on "not in play / settling"
  if ! grep -q "not in play (no stage, loading, a closed screen or a menu) 15, settling after play began 5" "$out/on.err"; then
    echo "FAIL seed $seed: the play rule: $(grep -o 'without B [^;]*' "$out/on.err" | head -1)"; fail=1; fi
  if grep -q "hard timeouts: lock 0, picture end 0" "$out/on.err"; then :; else echo "FAIL seed $seed: hard timeouts with the play rule"; fail=1; fi
  run on few $c49 $cshape GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_MINSHARE=100 GXV_FE_TWOCORE_USEWIN=10 GXV_FE_TWOCORE_REST=8 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 \
    && same off on "rests (MINSHARE=100)"
  if ! grep -q "the second worker rests for" "$out/on.err" || ! grep -q "resting (B made too little" "$out/on.err"; then echo "FAIL seed $seed: no rest with MINSHARE=100"; fail=1; fi
  echo "    play rule: $(grep -o 'without B [^;]*' "$out/on.err" | head -1)" >> "$out/summary.txt"
  # ---- 6. the base tree's front end with the switch off
  if [ -n "${REF_ROOT:-}" ]; then
    run ref few-ref $vita FEW_ODD=0 GXV_FE_TWOCORE=0 && run off few $vita FEW_ODD=0 GXV_FE_TWOCORE=0 && same off ref "GXV_FE_TWOCORE=0 against $REF_ROOT"
  fi
  # ---- 7. sanitizers: the console's settings, the console's threads, starvation
  for wl in "FEW_ODD=0" "FEW_ODD=1 FEW_JPA=100 FEW_GAME=1 FEW_SCENE_DROP=5 FEW_SEGMOVE=4" "FEW_ODD=0 FEW_GAME=1 GXV_FE_TWOCORE_INJECT=3" \
            "$cshape GXV_CHARAA_AUTO=0 GXV_SHADOW_RECT=0" "$cshape GXV_CHARAA_AUTO=0 GXV_SHADOW_RECT=0 GXV_FE_TWOCORE_INJECT=4 GXV_FE_TWOCORE_SLOTS=32 GXV_FE_TWOCORE_LEAD=1"; do
    run off few $console $wl GXV_FE_TWOCORE=0
    WATCH=60 run asan few-asan $console $wl GXV_FE_TWOCORE=1 || { grep -A20 "ERROR\|runtime error" "$out/asan.err" | head -30; }
    same off asan "the ASan/UBSan build, $wl"
    WATCH=60 run tsan few-tsan $console $wl GXV_SPLIT_JITTER=1 GXV_FE_TWOCORE=1 TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
      || { grep -A16 "WARNING: ThreadSanitizer" "$out/tsan.err" | head -40; }
    same off tsan "the TSan build, $wl"
  done
  echo "seed $seed: done ($pass_runs equal runs so far; the console's settings: taken per picture $t; $verline)"
done
if [ -n "${REPRO25_BIN:-}" ]; then
  # (for the record, twocore25: a fixture binary built from the front end before twocore25 (base 5dfbf8c) with this fixture, on the console's
  # stream shape with c49's settings: the console's verify failure, "ends n (most differing)" with "resolved state fields 0x6", and "empty")
  seed=1
  env $c49 $cshape GXV_FE_TWOCORE=2 GXV_FE_TWOCORE_LOG=100000 FEW_LOG=1 FEW_HASHAB=1 timeout 120 "$REPRO25_BIN" "$frames" 1 > "$out/repro25.txt" 2> "$out/repro25.err"
  echo "REPRO25_BIN (before twocore25), =2 on the console's stream shape: exit $?; $(grep "fetwo: GXV" "$out/repro25.err" | head -1 | grep -o 'empty [0-9.]*\|tiny [0-9.]*\|verify: [^;]*' | tr '\n' ' '); $(grep -m1 'fetwo: MISMATCH' "$out/repro25.err")"
fi
if [ -n "${REPRO_ROOT:-}" ]; then
  # (for the record: the tree before twocore24 in the console's threads: B's spin at 0x93 above the worker = the console's freeze)
  rsrcs=(tests/vgx_fe_work.cpp); for f in "${fe[@]}" "${two[@]}"; do rsrcs+=("$REPRO_ROOT/$f"); done
  if $cxx "${common[@]/#-I$S/-I$REPRO_ROOT/$S}" -O2 "${rsrcs[@]}" -o "$out/few-repro" > "$out/build-repro.log" 2>&1; then
    seed=1
    # (the console's settings before twocore24's: GXV_SHADOW_RECT=1 made that tree refuse every picture)
    env $vita FEW_ODD=0 FEW_GAME=1 FEW_SEG_GAP=1 FEW_GAME_RT=1 GXV_FE_TWOCORE=1 GXV_FE_TWOCORE_PRIO=0x93 timeout 20 stdbuf -oL "$out/few-repro" "$frames" 1 > "$out/repro1.txt" 2> /dev/null
    echo "REPRO_ROOT, B at 0x93 above the worker (SCHED_FIFO): exit $? after $(wc -l < "$out/repro1.txt") of $frames pictures (124 = still frozen after 20 s)"
    env $vita FEW_ODD=0 FEW_GAME=1 GXV_FE_TWOCORE=1 timeout 20 stdbuf -oL "$out/few-repro" "$frames" 1 > "$out/repro2.txt" 2> "$out/repro2.err"
    echo "REPRO_ROOT, segments next to each other (gx_fifo.cpp's two 2 MiB allocations): exit $? after $(wc -l < "$out/repro2.txt") pictures; $(grep -m1 'unknown GX\|went past' "$out/repro2.err")"
  else
    echo "(REPRO_ROOT: the build failed, see $out/build-repro.log)"
  fi
fi
cat "$out/summary.txt"
[ $fail = 0 ] && echo "PASS: GXV_FE_TWOCORE=1 and =2 give GXV_FE_TWOCORE=0's command buffers in $pass_runs runs (plain loop: 10 workloads x 7 settings, the console's stream shape; the console's threads with scene drops and segment moves; starvation; real-time priorities; the fall-back injections); verify clean; ASan/UBSan and TSan clean (synthetic J3D-shaped scene; not console timing)"
exit $fail

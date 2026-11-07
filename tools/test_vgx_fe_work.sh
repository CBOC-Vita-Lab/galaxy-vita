#!/usr/bin/env bash
# GXV_SLDELTA (vgx_state.cpp "register deltas") and GXV_RSMEMO (vgx_fesplit.cpp, the matrix rows a draw reads) on the host: the J3D-shaped
# scene of tests/vgx_fe_work.cpp (material lists in J3D's command order, double-buffered differed lists written again every picture,
# vertex-format lists, matrix loads, textures, plus "odd" state lists of random shape) through the real front end.
# Passes when, for each setting of the switches around them (GXV_SPLIT=0/1, GXV_BPFAST=0, GXV_REGSHADOW=0, GXV_RSCACHE=0, GXV_FE_KNOBS=0,
# the FE breakdown GXV_FE_SPLIT=2, the PS Vita
# defaults GXV_FIX_TEVIN / GXV_FS_HALF / GXV_FIX_LODBIAS off), GXV_SLDELTA=0/1/2 and GXV_RSMEMO=0/1 give the same command buffers frame for
# frame (and with GXV_FECUT_SLHOT=1 GXV_FECUT_KEYID=1 GXV_FECUT_HINT=1, the fecut change); GXV_SLDELTA=2 (each delta replay against SlReplay on a copy of the State, the compact deltas
# included) finds no mismatch; deltas and refusals both happen; and
# an AddressSanitizer / UndefinedBehaviorSanitizer build and a ThreadSanitizer build (split, jitter) report nothing.
# REF_ROOT=<a source tree without the change> (optional): the fixture is also built against that tree's front end, and its command buffers
# must be this tree's (the parts that are not switched: TevOorUpdate's single-register test, the vertex-descriptor mask).
#   tools/test_vgx_fe_work.sh [frames] [seeds]      (CXX: host compiler, default g++; build in $OUT, default /tmp/vgx_fe_work)
# Instruction counts (callgrind, the player's thread):  FEW_PROFILE=1 FEW_ODD=0 GXV_SPLIT=1 valgrind --tool=callgrind $OUT/few 200 1
# (FEW_JPA=<n>: n particles a picture in JParticle's command order; FEW_LOG=1: the front end's log lines, e.g. GXV_SPLIT=2's verify summary)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-120}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_fe_work}
mkdir -p "$out"
cxx=${CXX:-g++}
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 "${srcs[@]}" -o "$out/few"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "${srcs[@]}" -o "$out/few-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread "${srcs[@]}" -o "$out/few-tsan"
if [ -n "${REF_ROOT:-}" ]; then
  refsrcs=(tests/vgx_fe_work.cpp)
  for f in "${srcs[@]:1}"; do refsrcs+=("$REF_ROOT/$f"); done
  $cxx "${common[@]}" -I"$REF_ROOT/platform/vgx/src" -O2 "${refsrcs[@]}" -o "$out/few-ref"
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_RSCACHE=0" "GXV_FIX_TEVIN=0 GXV_FS_HALF=0 GXV_FIX_LODBIAS=0" "GXV_STATELIST=2" \
                "GXV_FE_KNOBS=0" "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    env $around GXV_SLDELTA=0 GXV_RSMEMO=0 "$out/few" "$frames" "$seed" > "$out/base.$seed.txt" 2> "$out/base.$seed.err"
    for cfg in "GXV_SLDELTA=1 GXV_RSMEMO=0" "GXV_SLDELTA=0 GXV_RSMEMO=1" "GXV_SLDELTA=1 GXV_RSMEMO=1" "GXV_SLDELTA=2 GXV_RSMEMO=1" "FEW_NONE=1" \
               "GXV_FECUT_SLHOT=1 GXV_FECUT_KEYID=1 GXV_FECUT_HINT=1" "GXV_SLDELTA=2 GXV_FECUT_SLHOT=1 GXV_FECUT_KEYID=1"; do
      env $around $cfg "$out/few" "$frames" "$seed" > "$out/run.$seed.txt" 2> "$out/run.$seed.err"
      if ! cmp -s "$out/base.$seed.txt" "$out/run.$seed.txt"; then
        echo "FAIL seed $seed: $around $cfg differs from GXV_SLDELTA=0 GXV_RSMEMO=0"; diff "$out/base.$seed.txt" "$out/run.$seed.txt" | head -4; fail=1
      fi
      if grep -q "mismatching [1-9]" "$out/run.$seed.err"; then echo "FAIL seed $seed: $around $cfg: $(tail -1 "$out/run.$seed.err")"; fail=1; fi
    done
  done
  # (the deltas must be used, and both outcomes of the compiler must occur)
  GXV_SLDELTA=2 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/count.$seed.err"
  set -- $(sed -n 's/.*checked \([0-9]*\), mismatching \([0-9]*\); lists compiled \([0-9]*\), refused \([0-9]*\).*/\1 \2 \3 \4/p' "$out/count.$seed.err")
  if [ "${1:-0}" -lt 1000 ] || [ "${2:-1}" != 0 ] || [ "${3:-0}" -lt 10 ] || [ "${4:-0}" -lt 1 ]; then
    echo "FAIL seed $seed: delta replays checked ${1:-?}, mismatching ${2:-?}, compiled ${3:-?}, refused ${4:-?}"; fail=1
  fi
  if [ -n "${REF_ROOT:-}" ]; then
    for sp in 0 1; do
      GXV_SPLIT=$sp "$out/few-ref" "$frames" "$seed" > "$out/pristine.$seed.txt" 2> /dev/null
      GXV_SPLIT=$sp "$out/few" "$frames" "$seed" > "$out/now.$seed.txt" 2> /dev/null
      if ! cmp -s "$out/pristine.$seed.txt" "$out/now.$seed.txt"; then
        echo "FAIL seed $seed: GXV_SPLIT=$sp differs from the front end of $REF_ROOT"; diff "$out/pristine.$seed.txt" "$out/now.$seed.txt" | head -4; fail=1
      fi
    done
  fi
  # GXV_FXJOIN=1 (vgx_fesplit.cpp, runs of jobs): the same command buffers as =0, with the particles of FEW_JPA (JParticle's command order)
  # and FEW_FX, split and split-verified; the runs must happen.
  for wl in "FEW_JPA=200" "FEW_JPA=150 FEW_FX=100"; do
    for sp in 1 2; do
      env $wl GXV_SPLIT=$sp GXV_FXJOIN=0 "$out/few" "$frames" "$seed" > "$out/fx0.$seed.txt" 2> /dev/null
      env $wl GXV_SPLIT=$sp GXV_FXJOIN=1 "$out/few" "$frames" "$seed" > "$out/fx1.$seed.txt" 2> "$out/fx1.$seed.err"
      if ! cmp -s "$out/fx0.$seed.txt" "$out/fx1.$seed.txt"; then
        echo "FAIL seed $seed: $wl GXV_SPLIT=$sp: GXV_FXJOIN=1 differs from =0"; diff "$out/fx0.$seed.txt" "$out/fx1.$seed.txt" | head -4; fail=1
      fi
      if ! grep -q "job runs (GXV_FXJOIN): [1-9]" "$out/fx1.$seed.err"; then echo "FAIL seed $seed: $wl: no job runs"; fail=1; fi
    done
    if [ -n "${REF_ROOT:-}" ]; then
      env $wl GXV_SPLIT=1 "$out/few-ref" "$frames" "$seed" > "$out/fxref.$seed.txt" 2> /dev/null
      if ! cmp -s "$out/fxref.$seed.txt" "$out/fx1.$seed.txt"; then echo "FAIL seed $seed: $wl: GXV_FXJOIN=1 differs from the front end of $REF_ROOT"; fail=1; fi
    fi
    if ! env $wl TSAN_OPTIONS="halt_on_error=1" GXV_SPLIT_JITTER=1 GXV_SPLIT=1 GXV_FXJOIN=1 "$out/few-tsan" "$((frames / 4))" "$seed" > /dev/null 2> "$out/tsanfx.$seed.err"; then
      echo "FAIL seed $seed: ThreadSanitizer, $wl GXV_FXJOIN=1"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsanfx.$seed.err" | head -30; fail=1
    fi
    if ! env $wl GXV_SPLIT=1 GXV_FXJOIN=1 "$out/few-asan" "$((frames / 2))" "$seed" > /dev/null 2> "$out/asanfx.$seed.err"; then
      echo "FAIL seed $seed: sanitizers, $wl GXV_FXJOIN=1"; head -30 "$out/asanfx.$seed.err"; fail=1
    fi
  done
  for sl in 0 1 2; do
    if ! GXV_SLDELTA=$sl GXV_SPLIT=1 "$out/few-asan" "$((frames / 2))" "$seed" > "$out/asan.$sl.$seed.txt" 2> "$out/asan.$seed.err"; then
      echo "FAIL seed $seed: sanitizers, GXV_SLDELTA=$sl"; head -30 "$out/asan.$seed.err"; fail=1
    fi
  done
  if ! cmp -s "$out/asan.0.$seed.txt" "$out/asan.1.$seed.txt" || ! cmp -s "$out/asan.0.$seed.txt" "$out/asan.2.$seed.txt"; then
    echo "FAIL seed $seed: sanitizer build, GXV_SLDELTA=0/1/2 differ"; fail=1
  fi
  if ! TSAN_OPTIONS="halt_on_error=1" GXV_SPLIT_JITTER=1 GXV_SPLIT=1 "$out/few-tsan" "$((frames / 4))" "$seed" > "$out/tsan.$seed.txt" 2> "$out/tsan.$seed.err"; then
    echo "FAIL seed $seed: ThreadSanitizer, GXV_SPLIT=1"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsan.$seed.err" | head -30; fail=1
  fi
  # GXV_FECUT_* (the shader keys kept by id on the finishing side, the compact deltas, the hints): sanitizers, and the split's threads
  if ! GXV_FECUT_SLHOT=1 GXV_FECUT_KEYID=1 GXV_FECUT_HINT=1 GXV_SPLIT=1 "$out/few-asan" "$((frames / 2))" "$seed" > "$out/asancut.$seed.txt" 2> "$out/asancut.$seed.err"; then
    echo "FAIL seed $seed: sanitizers, GXV_FECUT_*=1"; head -30 "$out/asancut.$seed.err"; fail=1
  fi
  if ! cmp -s "$out/asan.0.$seed.txt" "$out/asancut.$seed.txt"; then echo "FAIL seed $seed: sanitizer build, GXV_FECUT_*=1 differs"; fail=1; fi
  if ! TSAN_OPTIONS="halt_on_error=1" GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_SPLIT_JITTER=1 GXV_SPLIT=1 "$out/few-tsan" "$((frames / 4))" "$seed" > "$out/tsancut.$seed.txt" 2> "$out/tsancut.$seed.err"; then
    echo "FAIL seed $seed: ThreadSanitizer, GXV_FECUT_KEYID=1"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsancut.$seed.err" | head -30; fail=1
  fi
  echo "seed $seed: $(wc -l < "$out/base.$seed.txt") frames compared; $(tail -1 "$out/count.$seed.err")"
done
[ $fail = 0 ] && echo "PASS: GXV_SLDELTA=0/1/2, GXV_RSMEMO=0/1 and GXV_FECUT_SLHOT/KEYID/HINT=1 identical (GXV_SPLIT=0/1, BPFAST/REGSHADOW/RSCACHE/STATELIST=2, Vita fixes off); verify clean; sanitizers clean (synthetic J3D-shaped scene, not Vita timing)"
exit $fail

#!/usr/bin/env bash
# GXV_FXMERGE on the host: JParticle's particle loops sent as port-private commands (decomp/src/RVL_SDK/gx/GXDisplayList.c, the
# gathering; platform/vgx/src/vgx_state.cpp FxMergeRun, the front end's side) must give the front end's command buffers, vertex arena,
# indices and uniform blocks exactly as the separate GX commands give them.
#   1. tests/vgx_fe_work.cpp (the real front end on a J3D-shaped scene plus FEW_JPA particles in JParticle's command order, FEW_JPAVAR:
#      the other kinds of emitters, FEW_FXCAP: commands cut short): FEW_FXMERGE=1 against =0, frame by frame, for each setting of the
#      switches around it (split 0/1/2, folding, CPU transform, texture bake, the tiny-list size, the geometry cache, the resolution
#      cache and its verify, BPFAST, REGSHADOW, FXJOIN, NOMERGE, FOLDAHEAD and its verify, RIGIDMERGE, the decode and its verify, the
#      FE breakdown); the verify modes must log no mismatch; the short way, the decode reuse and CallList's way must all be taken.
#   2. tests/vgx_fxmerge.cpp + tests/vgx_fxmerge_fe.cpp: the production GX units (GXTev.c, GXTransform.c, GXDisplayList.c,
#      GXGeometry.c) and gx_fifo.cpp drive the real front end; GXV_FXMERGE=1 against =0 (split 0/1/2); a stray GX write inside a
#      particle loop is noticed, logged, and the gathering stays off afterwards.
#   3. AddressSanitizer / UndefinedBehaviorSanitizer and ThreadSanitizer (split, jitter) builds of 1, with FEW_FXMERGE=1.
# REF_ROOT=<a source tree without the change> (optional): FEW_FXMERGE=1 here must also match that tree's front end given the separate
# commands (FEW_FXMERGE=0: it does not know the command).
#   tools/test_vgx_fxmerge.sh [frames] [seeds]      (build in $OUT, default /tmp/vgx_fxmerge)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-40}
seeds=${2:-"1 2 3"}
out=${OUT:-/tmp/vgx_fxmerge}
mkdir -p "$out"
cxx=${CXX:-g++}
fe=(platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp
    platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp
    platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
$cxx "${common[@]}" -O2 tests/vgx_fe_work.cpp "${fe[@]}" -o "$out/few"
$cxx "${common[@]}" -O1 -fsanitize=address,undefined -fno-omit-frame-pointer tests/vgx_fe_work.cpp "${fe[@]}" -o "$out/few-asan"
${TSAN_CXX:-g++} "${common[@]}" -O1 -fsanitize=thread tests/vgx_fe_work.cpp "${fe[@]}" -o "$out/few-tsan"
if [ -n "${REF_ROOT:-}" ]; then
  ref=()
  for f in "${fe[@]}"; do ref+=("$REF_ROOT/$f"); done
  $cxx "${common[@]}" -I"$REF_ROOT/platform/vgx/src" -O2 tests/vgx_fe_work.cpp "${ref[@]}" -o "$out/few-ref"
fi
# (the end-to-end test: the GX units as C, the game half with gx_fifo.cpp as tests/xfburst.cpp builds it, the front-end half as the fixture)
inc=(-Iplatform/include -Idecomp/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include)
gxo=()
for u in GXDisplayList GXTev GXTransform GXGeometry; do
  ${CC:-clang} -O2 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include platform/include/port/compat.h -w \
      -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -c "decomp/src/RVL_SDK/gx/$u.c" -o "$out/$u.o"
  gxo+=("$out/$u.o")
done
${GAME_CXX:-clang++} -std=c++20 -O1 -g -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
    -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 "${inc[@]}" -Iplatform/src/gx -Wno-register -Wno-c++11-narrowing \
    -Wno-format -Wno-unknown-pragmas -Wno-header-guard -ffunction-sections -fdata-sections -c tests/vgx_fxmerge.cpp -o "$out/fxm-game.o"
$cxx "${common[@]}" -O2 tests/vgx_fxmerge_fe.cpp "${fe[@]}" "$out/fxm-game.o" "${gxo[@]}" -Wl,--gc-sections -Wl,--allow-multiple-definition -o "$out/fxm"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
same() { # $1: what; $2, $3: the outputs
  if ! cmp -s "$2" "$3"; then echo "FAIL $1"; diff "$2" "$3" | head -4; fail=1; fi
}
counts() { sed -n 's/.*particle commands (GXV_FXMERGE): \([0-9]*\) commands, [0-9]* items, \([0-9]*\) list calls, \([0-9]*\) short, \([0-9]*\) decodes reused, \([0-9]*\) through CallList.*/\1 \2 \3 \4 \5/p' "$1"; }
for seed in $seeds; do
  for around in "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_SPLIT=2 FEW_LOG=1" "GXV_FOLDREGS=0" "GXV_SOFTXFORM=0" "GXV_TEXBAKE=0" "GXV_DL_TINY=15" "GXV_DLCACHE=0" \
                "GXV_RSCACHE=0" "GXV_RSCACHE=2 FEW_LOG=1" "GXV_BPFAST=0" "GXV_BPFAST=2 FEW_LOG=1" "GXV_REGSHADOW=0" "GXV_FXJOIN=1" "GXV_NOMERGE=1" \
                "GXV_FOLDAHEAD=0" "GXV_FOLDAHEAD=2 FEW_LOG=1" "GXV_RIGIDMERGE=0" "GXV_DECODE=0" "GXV_DECODE=2 FEW_LOG=1" "GXV_FE_KNOBS=0" \
                "GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=2"; do
    for wl in "FEW_JPA=300 FEW_JPAVAR=1" "FEW_JPA=200 FEW_FX=60 FEW_JPAVAR=1 FEW_FXCAP=60" "FEW_JPA=150 FEW_ODD=0"; do
      env $around $wl FEW_FXMERGE=0 "$out/few" "$frames" "$seed" > "$out/a.$seed.txt" 2> "$out/a.$seed.err"
      env $around $wl FEW_FXMERGE=1 "$out/few" "$frames" "$seed" > "$out/b.$seed.txt" 2> "$out/b.$seed.err"
      same "seed $seed: $around $wl: FEW_FXMERGE=1 differs from =0" "$out/a.$seed.txt" "$out/b.$seed.txt"
      if grep -qE "MISMATCH|mismatching [1-9]|(^|[^0-9])[1-9][0-9]* (mismatching|frames with mismatches)" "$out/b.$seed.err"; then
        echo "FAIL seed $seed: $around $wl: a verify mode logged a mismatch"; grep -E "MISMATCH|mismatch" "$out/b.$seed.err" | head -3; fail=1
      fi
      # (the command only ever stands in the top-level stream; the scene's random "odd" lists may hold the byte 0x58 as before)
      if grep -q "fxmerge: unknown item\|unknown GX opcode 0x58 in a command stream" "$out/b.$seed.err"; then echo "FAIL seed $seed: $around $wl: an item or the command was not known"; fail=1; fi
    done
  done
  # (every way of an item must be taken: short draws, reused decodes, CallList's way)
  FEW_JPA=300 FEW_JPAVAR=1 FEW_FXMERGE=1 GXV_SPLIT=1 "$out/few" "$frames" "$seed" > /dev/null 2> "$out/c.$seed.err"
  set -- $(counts "$out/c.$seed.err")
  if [ "${1:-0}" -lt 100 ] || [ "${3:-0}" -lt 1000 ] || [ "${4:-0}" -lt 1000 ] || [ "${5:-0}" -lt 10 ]; then
    echo "FAIL seed $seed: commands ${1:-?}, calls ${2:-?}, short ${3:-?}, reused ${4:-?}, through CallList ${5:-?}"; fail=1
  fi
  echo "seed $seed: particle commands ${1:-?}, list calls ${2:-?}: short ${3:-?} (decode reused ${4:-?}), through CallList ${5:-?}"
  if [ -n "${REF_ROOT:-}" ]; then
    for sp in 0 1; do
      FEW_JPA=300 FEW_JPAVAR=1 FEW_FXMERGE=0 GXV_SPLIT=$sp "$out/few-ref" "$frames" "$seed" > "$out/r.$seed.txt" 2> /dev/null
      FEW_JPA=300 FEW_JPAVAR=1 FEW_FXMERGE=1 GXV_SPLIT=$sp "$out/few" "$frames" "$seed" > "$out/b.$seed.txt" 2> /dev/null
      same "seed $seed: GXV_SPLIT=$sp: FEW_FXMERGE=1 differs from the front end of $REF_ROOT" "$out/r.$seed.txt" "$out/b.$seed.txt"
      GXV_SPLIT=$sp "$out/few-ref" "$frames" "$seed" > "$out/r.$seed.txt" 2> /dev/null
      GXV_SPLIT=$sp "$out/few" "$frames" "$seed" > "$out/b.$seed.txt" 2> /dev/null
      same "seed $seed: GXV_SPLIT=$sp: the scene without particles differs from the front end of $REF_ROOT" "$out/r.$seed.txt" "$out/b.$seed.txt"
    done
  fi
  for sp in 0 1 2; do
    GXV_SPLIT=$sp GXV_FXMERGE=0 "$out/fxm" "$frames" "$seed" > "$out/g0.$seed.txt" 2> /dev/null
    GXV_SPLIT=$sp GXV_FXMERGE=1 "$out/fxm" "$frames" "$seed" > "$out/g1.$seed.txt" 2> "$out/g1.$seed.err"
    same "seed $seed: GX units end to end, GXV_SPLIT=$sp: GXV_FXMERGE=1 differs from =0" "$out/g0.$seed.txt" "$out/g1.$seed.txt"
    if ! grep -q "gathering on" "$out/g1.$seed.err" || grep -q "inside a particle loop" "$out/g1.$seed.err"; then echo "FAIL seed $seed: the gathering did not stay on"; fail=1; fi
  done
  echo "seed $seed: end to end: $(tail -1 "$out/g1.$seed.err")"
  FXT_FOREIGN=1 GXV_FXMERGE=1 "$out/fxm" 4 "$seed" > /dev/null 2> "$out/gf.$seed.err"
  if ! grep -q "inside a particle loop" "$out/gf.$seed.err" || ! grep -q "gathering off at the end" "$out/gf.$seed.err"; then
    echo "FAIL seed $seed: a stray write inside a particle loop was not noticed"; fail=1
  fi
  for wl in "FEW_JPA=300 FEW_JPAVAR=1" "FEW_JPA=200 FEW_FX=60 FEW_JPAVAR=1 FEW_FXCAP=60"; do
    if ! env $wl FEW_FXMERGE=1 GXV_SPLIT=1 "$out/few-asan" "$((frames / 2))" "$seed" > /dev/null 2> "$out/asan.$seed.err"; then
      echo "FAIL seed $seed: sanitizers, $wl"; head -30 "$out/asan.$seed.err"; fail=1
    fi
    if ! env $wl TSAN_OPTIONS="halt_on_error=1" GXV_SPLIT_JITTER=1 GXV_SPLIT=1 FEW_FXMERGE=1 "$out/few-tsan" "$((frames / 4))" "$seed" > /dev/null 2> "$out/tsan.$seed.err"; then
      echo "FAIL seed $seed: ThreadSanitizer, $wl"; grep -A12 "WARNING: ThreadSanitizer" "$out/tsan.$seed.err" | head -30; fail=1
    fi
  done
done
[ $fail = 0 ] && echo "PASS: FEW_FXMERGE=1 / GXV_FXMERGE=1 identical to the separate commands (fixture: 22 settings x 3 workloads; GX units end to end, split 0/1/2); verify modes clean; stray write noticed; sanitizers clean (host, not Vita timing)"
exit $fail

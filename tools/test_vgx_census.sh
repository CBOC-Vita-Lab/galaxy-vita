#!/usr/bin/env bash
# lighter25: GXV_STREAM_CENSUS (platform/vgx/src/vgx_census.inc) and GXV_ARRAYSKIP=2's front-end check on the J3D-shaped host fixture
# (tests/vgx_fe_work.cpp) with the console's stream shape and build c49's settings. Passes when
#   - the command buffers are the same frame for frame with the census off, on for every picture and on for one in 3, with and without the
#     game's census markers in the stream (FEW_CENSUS), for several workloads (the console shape; particles through FXMERGE; big streamed draws
#     and shadow volumes drawn inline; fur), seeds 1 and 2;
#   - in every census line the kinds add up to the bytes parsed, and over the run the census's bytes plus the BEGIN markers (5 bytes each,
#     before the census turns on) are the fixture's stream bytes exactly;
#   - the lighter streams of FEW_LIGHTER (a vertex-format call, a differed list, array-base / CP 0x30 writes that change nothing, left out at
#     the source) give the same command buffers (FEW_J3DFMT=1: J3D's own vertex-format lists);
#   - GXV_ARRAYSKIP=2's markers (FEW_LIGHTER=8) are checked by the front end with none differing, and a wrong one (FEW_LIGHTER=24) is seen;
#   - an AddressSanitizer + UndefinedBehaviorSanitizer build of the census and lighter runs reports nothing.
#   tools/test_vgx_census.sh [frames]     (OUT=<dir>, default /tmp/vgx_census; NOBUILD=1 reuses the binaries; CXX)
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-40}
out=${OUT:-/tmp/vgx_census}
mkdir -p "$out"
cxx=${CXX:-g++}
S=platform/vgx/src
srcs=(tests/vgx_fe_work.cpp $S/vgx_state.cpp $S/vgx_draw.cpp $S/vgx_fesplit.cpp $S/vgx_dlcache.cpp $S/vgx_guestmem.cpp $S/vgx_bpsync.cpp
      $S/vgx_shadergen.cpp $S/vgx_texcache.cpp $S/vgx_texcomp.cpp $S/vgx_texdecode.cpp $S/vgx_stats.cpp $S/vgx_backend_null.cpp
      $S/vgx_twocore_b.cpp $S/vgx_twocore_b_state.cpp $S/vgx_twocore_b_draw.cpp $S/vgx_twocore_b_dlcache.cpp $S/vgx_twocore_b_fesplit.cpp
      $S/vgx_twocore_b_bpsync.cpp $S/vgx_twocore_b_shadergen.cpp $S/vgx_twocore_b_stats.cpp)
cd "$root"
build() { # name, flags...
  local name=$1; shift
  if ! "$cxx" -std=c++17 -g -I$S -Iplatform/vgx/third_party -pthread -DFEW_VITA_YIELD -Wl,--wrap=sched_yield "$@" "${srcs[@]}" -o "$out/$name" > "$out/build-$name.log" 2>&1; then
    echo "FAIL: the build of $name failed:"; grep -E "error|undefined reference" "$out/build-$name.log" | head -20; exit 2
  fi
}
if [ -z "${NOBUILD:-}" ]; then
  build few -O2
  build few-asan -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
vita="GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1
      GXV_STATICCULL_PAL=1 GXV_DYNCULL=1 GXV_DLWELD=1 GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24
      GXV_FE_PARALLEL=1"
c49="$vita GXV_CHARAA_AUTO=0 GXV_SHADOW_RECT=0 GXV_TEVIN_CANON=2 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_TEX_SCENEDROP=3
     GXV_SL_SCENEDROP=2 FEW_RETAINED=1 FEW_ALPHA=1"
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
run() { # bin, out file, env...
  local bin=$1 o=$2; shift 2
  env $c49 FEW_HASHAB=1 FEW_LOG=1 "$@" "$out/$bin" "$frames" "$seed" > "$o" 2> "$o.err" || fail "run $bin $* exited $?"
  if grep -q "ERROR: AddressSanitizer\|runtime error" "$o.err"; then fail "sanitizer report: $bin $*"; fi
}
workloads=("FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150" "FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150 FEW_FXMERGE=1 GXV_FXMERGE=1"
           "FEW_BIG=8 FEW_SHADOW=4 FEW_SHADOWDYN=4 FEW_JPA=60" "FEW_FUR=8 FEW_PLANTS=40")
for seed in 1 2; do
  for wi in "${!workloads[@]}"; do
    w=${workloads[$wi]}
    run few "$out/ref.txt" $w
    for cen in "GXV_STREAM_CENSUS=0 FEW_CENSUS=3" "GXV_STREAM_CENSUS=1 FEW_CENSUS=1" "GXV_STREAM_CENSUS=3 FEW_CENSUS=3 GXV_STREAM_CENSUS_PRINT=2"; do
      run few "$out/c.txt" $w $cen
      cmp -s "$out/ref.txt" "$out/c.txt" || fail "buffers differ: seed $seed [$w] [$cen]"
      if [[ $cen == *"CENSUS=1 "* || $cen == *"CENSUS=3 FEW"* ]]; then
        n=$(grep -c "stream-census: .*counted pictures" "$out/c.txt.err")
        [ "$n" -gt 0 ] || fail "no census line: seed $seed [$w] [$cen]"
        grep "stream-census: .*counted pictures" "$out/c.txt.err" | grep -q "NOT EQUAL" && fail "kinds do not add up: seed $seed [$w] [$cen]"
      fi
      if [[ $cen == *"CENSUS=1 FEW_CENSUS=1"* ]]; then
        # (one window over the run: every picture counted; the census's bytes + 5 per BEGIN marker = the stream's bytes)
        run few "$out/w.txt" $w $cen GXV_STREAM_CENSUS_PRINT=$frames
        tot=$(grep -o "window total [0-9]*" "$out/w.txt.err" | awk '{s+=$3} END {print s}')
        str=$(grep -o "[0-9]* stream bytes" "$out/w.txt.err" | awk '{print $1}')
        [ "$((tot + 5 * frames))" = "$str" ] || fail "census bytes $tot + $((5 * frames)) != stream bytes $str: seed $seed [$w]"
        echo "seed $seed [$w]: census $tot + markers $((5 * frames)) = stream $str" >> "$out/summary.txt"
      fi
    done
  done
  # (the lighter streams: the same pictures)
  w="FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150 FEW_J3DFMT=1"
  run few "$out/ref.txt" $w
  for l in 1 2 4 7 8; do
    run few "$out/l.txt" $w FEW_LIGHTER=$l
    cmp -s "$out/ref.txt" "$out/l.txt" || fail "lighter stream FEW_LIGHTER=$l changes the buffers: seed $seed"
  done
  grep -q "arrayskip verify: [1-9][0-9]* checked, 0 differing" "$out/l.txt.err" || fail "FEW_LIGHTER=8: no clean arrayskip check (seed $seed): $(grep 'arrayskip verify' "$out/l.txt.err" | tail -1)"
  run few "$out/l.txt" $w FEW_LIGHTER=24
  grep -q "arrayskip verify: [0-9]* checked, [1-9][0-9]* differing" "$out/l.txt.err" || fail "FEW_LIGHTER=24: the wrong markers were not seen (seed $seed)"
  # (sanitizers: census and lighter streams)
  run few-asan "$out/a1.txt" FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150 GXV_STREAM_CENSUS=1 FEW_CENSUS=1 GXV_STREAM_CENSUS_PRINT=2
  run few-asan "$out/a2.txt" FEW_BIG=8 FEW_SHADOW=4 FEW_SHADOWDYN=4 FEW_JPA=60 FEW_FXMERGE=1 GXV_FXMERGE=1 GXV_STREAM_CENSUS=2 FEW_CENSUS=2
  run few-asan "$out/a3.txt" $w FEW_LIGHTER=15
  run few-asan "$out/a1r.txt" FEW_ODD=0 FEW_CONSOLE=1 FEW_JPA=150
  cmp -s "$out/a1.txt" "$out/a1r.txt" || fail "the sanitizer build: the census changes the buffers (seed $seed)"
done
if [ "$fails" = 0 ]; then echo "PASS"; else echo "FAIL ($fails)"; exit 1; fi

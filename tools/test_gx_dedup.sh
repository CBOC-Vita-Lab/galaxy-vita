#!/usr/bin/env bash
# GXV_GX_DEDUP / GXV_GX_DEDUP_CENSUS (gxdedup31) on the host: the producer-side removal of repeated register writes must leave the front
# end's pictures and State exactly as they are. Two harnesses, both through the real front end:
#   1. tests/vgx_fe_work.cpp FEW_GXDEDUP: the J3D-shaped scene (material / differed / vertex-format / geometry lists, odd lists, particles,
#      shadow volumes, big streamed draws, J3D brackets, retained handles, census markers, plus FEW_GXDEDUP_RAND's random top-level writes:
#      masks, TEV colour / konst halves, matrix indices on CP and XF, unused arrays, a copy trigger sent twice) with its top-level stream
#      sent through the game's decision core (platform/include/port/gx_dedup_core.h). For every seed, workload and setting of the front
#      end's own switches around it: FEW_GXDEDUP=0 / 1 / 2 give the command buffers of the plain run, picture for picture; the State hash
#      at every top-level draw and list call (FEW_GXDEDUP_STATE=1) is the same for 0, 1 and 2; =2's probes (every one with a digest,
#      GXV_GX_DEDUP_DIGEST=1) find nothing differing; repeats were left out. Negative controls: the core's four mutants (a side-effect
#      register taken as plain, the pending mask ignored, invalidations ignored, every list taken as primitives) must change the pictures
#      or the State under =1 and make =2's probes differ.
#   2. tests/gx_dedup_e2e.cpp + tests/gx_dedup_fe.cpp: the production GX units (GXTev, GXPixel, GXAttr, GXGeometry with the real dirty-state
#      flush, GXLight, GXTransform, GXDisplayList, GXMisc, GXBump, GXTexture) and gx_fifo.cpp with the gate (platform/src/gx/gx_dedup.inc):
#      GXV_GX_DEDUP=0 / 1 / 2 and GXV_GX_DEDUP_CENSUS give the same pictures; the census's stream bytes are =0's; =1's stream is shorter;
#      =2's probes find nothing differing; the census prints its lines; the short path (=1) writes the stream the general path
#      (GXV_GX_DEDUP_SHORT=0) writes; GXV_GX_DEDUP_CLASSES=1 / 2 (one class gated) give =0's pictures, a stream between =0's and =1's
#      and clean probes. Negative controls: a caller vouching wrongly for a list
#      (GXDD_E2E_MUTANT=1 / 2) must change =1's pictures and make =2's probes differ.
#   3. AddressSanitizer / UndefinedBehaviorSanitizer builds of both, a subset of the runs.
#   tools/test_gx_dedup.sh [frames] [seeds]      (build in $OUT, default $MP_TMP/gx_dedup; host compilers CXX / GAME_CXX / CC)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-24}
seeds=${2:-"1 2 3"}
out=${OUT:-$MP_TMP/gx_dedup}
mkdir -p "$out/o" "$out/oa"
cxx=${CXX:-g++}
cd "$root"
fe=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_null vgx_texcomp)
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/include -Iplatform/vgx/third_party -pthread)
asan=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
if [ -z "${SKIP_BUILD:-}" ]; then
for f in "${fe[@]}"; do # (one compile at a time: the machine is small)
  nice -n 10 "$cxx" "${common[@]}" -O2 -c "platform/vgx/src/$f.cpp" -o "$out/o/$f.o"
  nice -n 10 "$cxx" "${common[@]}" "${asan[@]}" -c "platform/vgx/src/$f.cpp" -o "$out/oa/$f.o"
done
nice -n 10 "$cxx" "${common[@]}" -O2 tests/vgx_fe_work.cpp "$out"/o/*.o -o "$out/few"
nice -n 10 "$cxx" "${common[@]}" "${asan[@]}" tests/vgx_fe_work.cpp "$out"/oa/*.o -o "$out/few-asan"
# (the end-to-end harness: the GX units as C, the game half with gx_fifo.cpp, the front-end half)
inc=(-Iplatform/include -Idecomp/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include)
gxu=(GXDisplayList GXTev GXTransform GXGeometry GXAttr GXPixel GXLight GXMisc GXBump GXTexture)
for v in plain asan off; do  # (off: built without PORT_GX_DEDUP_BUILD, as the release is: the writes as they were)
  san=(); [ "$v" = asan ] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  bld=(-DPORT_GX_DEDUP_BUILD=1); [ "$v" = off ] && bld=()
  gxo=()
  for u in "${gxu[@]}"; do
    nice -n 10 ${CC:-clang} -O1 "${san[@]}" "${bld[@]}" -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include platform/include/port/compat.h -w \
        -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -c "decomp/src/RVL_SDK/gx/$u.c" -o "$out/$v-$u.o"
    gxo+=("$out/$v-$u.o")
  done
  nice -n 10 ${CC:-clang} -O1 -w -c tests/gx_dedup_e2e_stubs.c -o "$out/$v-stubs.o"
  nice -n 10 ${GAME_CXX:-clang++} -std=c++20 -O1 -g "${san[@]}" "${bld[@]}" -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 \
      -DPORT_PLATFORM_CODE=1 -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 "${inc[@]}" -Iplatform/src/gx -Wno-register \
      -Wno-c++11-narrowing -Wno-format -Wno-unknown-pragmas -Wno-header-guard -ffunction-sections -fdata-sections -c tests/gx_dedup_e2e.cpp -o "$out/$v-game.o"
  if [ "$v" != asan ]; then fo=("$out"/o/*.o); fflags=(-O2); else fo=("$out"/oa/*.o); fflags=("${asan[@]}"); fi
  nice -n 10 "$cxx" "${common[@]}" "${fflags[@]}" -c tests/gx_dedup_fe.cpp -o "$out/$v-fe.o"
  nice -n 10 "$cxx" -pthread "${san[@]}" "$out/$v-fe.o" "${fo[@]}" "$out/$v-game.o" "${gxo[@]}" "$out/$v-stubs.o" -Wl,--gc-sections -Wl,--allow-multiple-definition \
      -o "$out/e2e-$v"
done
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0 runs=0
bad() { echo "FAIL $*"; fail=1; }
frames_of() { grep -v '^dd-state' "$1"; }
states_of() { grep '^dd-state' "$1" || true; }
verify_of() { grep -E 'gxdedup \(FEW_GXDEDUP|gxdedup e2e:' "$1" | sed -n 's/.*verify: \([0-9]*\) checked, \([0-9]*\) differing.*/\1 \2/p' | tail -1; }
works=("FEW_GXDEDUP_RAND=3 FEW_COPY=5"
       "FEW_J3N=1 FEW_JPA=200 FEW_JPAVAR=1 FEW_FX=40 FEW_BIG=6 FEW_SHADOW=4 FEW_SHADOWDYN=3 FEW_CONSOLE=1 FEW_J3DFMT=1 FEW_LIGHT=3 FEW_FOG=4 FEW_CENSUS=3 FEW_MIXED=6 FEW_PLANTS=8 FEW_TOP=60 FEW_GXDEDUP_RAND=2 FEW_COPY=7"
       "FEW_FXMERGE=1 FEW_JPA=150 FEW_FUR=3 FEW_INST=6 FEW_SCENE=7 FEW_GXDEDUP_RAND=2"
       "FEW_GXDEDUP_RAND=4 FEW_GXDEDUP_CLASS=0 FEW_TOP=40")
for seed in $seeds; do
  for w in "${works[@]}"; do
    for around in "" "GXV_SPLIT=0" "GXV_BPFAST=0" "GXV_REGSHADOW=0" "GXV_FE_STATEFAST=1" "GXV_NOOP_FILTER=1" "GXV_STATELIST=2"; do
      tag="seed $seed [$w] [$around]"
      env $around $w FEW_RETAINED=1 "$out/few" "$frames" "$seed" > "$out/plain.txt" 2> "$out/plain.err" || bad "$tag: plain run"
      for m in 0 1 2; do
        env $around $w FEW_RETAINED=1 FEW_GXDEDUP=$m FEW_GXDEDUP_STATE=1 GXV_GX_DEDUP_DIGEST=1 "$out/few" "$frames" "$seed" > "$out/m$m.txt" 2> "$out/m$m.err" || bad "$tag: FEW_GXDEDUP=$m run"
        runs=$((runs + 1))
        cmp -s <(frames_of "$out/plain.txt") <(frames_of "$out/m$m.txt") || { bad "$tag: FEW_GXDEDUP=$m pictures differ from the plain run"; diff <(frames_of "$out/plain.txt") <(frames_of "$out/m$m.txt") | head -4; }
      done
      cmp -s <(states_of "$out/m0.txt") <(states_of "$out/m1.txt") || bad "$tag: State at a draw / call differs, =1 against =0"
      cmp -s <(states_of "$out/m0.txt") <(states_of "$out/m2.txt") || bad "$tag: State at a draw / call differs, =2 against =0"
      [ "$(states_of "$out/m0.txt" | wc -l)" -eq "$frames" ] || bad "$tag: no State lines"
      set -- $(verify_of "$out/m2.err")
      [ "${1:-0}" -gt 0 ] && [ "${2:-1}" = 0 ] || bad "$tag: =2 verify: ${1:-?} checked, ${2:-?} differing"
      if grep -q "MISMATCH\|mismatching [1-9]" "$out/m2.err" "$out/m1.err"; then bad "$tag: a verify mode of the front end logged a mismatch"; fi
    done
  done
  echo "seed $seed: $(grep -o 'writes BP.*particle commands [0-9]*' "$out/m1.err" | head -1)"
done
# (the core's negative controls: each must show)
w="FEW_GXDEDUP_RAND=3 FEW_COPY=5 FEW_RETAINED=1"
env $w FEW_GXDEDUP=0 FEW_GXDEDUP_STATE=1 "$out/few" "$frames" 1 > "$out/ref.txt" 2> /dev/null
for mu in 1 2 3 4; do
  env $w FEW_GXDEDUP=1 FEW_GXDEDUP_STATE=1 FEW_GXDEDUP_MUTANT=$mu "$out/few" "$frames" 1 > "$out/mu.txt" 2> /dev/null
  env $w FEW_GXDEDUP=2 FEW_GXDEDUP_MUTANT=$mu GXV_GX_DEDUP_DIGEST=1 "$out/few" "$frames" 1 > /dev/null 2> "$out/mu2.err"
  set -- $(verify_of "$out/mu2.err")
  if cmp -s "$out/ref.txt" "$out/mu.txt"; then bad "fixture mutant $mu: =1 left the pictures and the State as they were"; fi
  [ "${2:-0}" -gt 0 ] || bad "fixture mutant $mu: =2 found nothing differing"
  echo "fixture mutant $mu: caught (=2: ${1:-?} checked, ${2:-?} differing)"
done
# (the end-to-end harness)
for seed in $seeds; do
  for m in 0 1 2 c; do
    if [ "$m" = c ]; then env GXV_GX_DEDUP_CENSUS=8 "$out/e2e-plain" "$frames" "$seed" > "$out/e$m.txt" 2> "$out/e$m.err" || bad "e2e seed $seed census run"
    else env GXV_GX_DEDUP=$m GXV_GX_DEDUP_DIGEST=1 "$out/e2e-plain" "$frames" "$seed" > "$out/e$m.txt" 2> "$out/e$m.err" || bad "e2e seed $seed =$m run"; fi
  done
  for m in 1 2 c; do cmp -s <(grep '^frame' "$out/e0.txt") <(grep '^frame' "$out/e$m.txt") || bad "e2e seed $seed: pictures of =$m differ from =0"; done
  cmp -s <(grep '^stream' "$out/e0.txt") <(grep '^stream' "$out/ec.txt") || bad "e2e seed $seed: the census changed the stream"
  b0=$(awk '/^stream/{s+=$4} END{print s+0}' "$out/e0.txt"); b1=$(awk '/^stream/{s+=$4} END{print s+0}' "$out/e1.txt")
  [ "$b1" -lt "$b0" ] || bad "e2e seed $seed: =1's stream ($b1 bytes) is not shorter than =0's ($b0)"
  set -- $(verify_of "$out/e2.err")
  [ "${1:-0}" -gt 0 ] && [ "${2:-1}" = 0 ] || bad "e2e seed $seed: =2 verify: ${1:-?} checked, ${2:-?} differing"
  grep -q "^gxdedup-census: call sites by identical" "$out/ec.err" || bad "e2e seed $seed: no census lines"
  echo "e2e seed $seed: stream $b0 -> $b1 bytes over $frames pictures; =2 verify ${1:-?} checked, ${2:-?} differing"
  # (the short path against the general path: the same stream, byte for byte)
  env GXV_GX_DEDUP=1 GXV_GX_DEDUP_SHORT=0 "$out/e2e-plain" "$frames" "$seed" > "$out/es.txt" 2> /dev/null || bad "e2e seed $seed SHORT=0 run"
  cmp -s <(grep '^stream' "$out/e1.txt") <(grep '^stream' "$out/es.txt") || bad "e2e seed $seed: the short path's stream differs from the general path's"
  # (one class gated: the pictures of =0, a shorter stream, =2's probes clean)
  for cl in 1 2; do
    env GXV_GX_DEDUP=1 GXV_GX_DEDUP_CLASSES=$cl "$out/e2e-plain" "$frames" "$seed" > "$out/ek1.txt" 2> /dev/null || bad "e2e seed $seed CLASSES=$cl =1 run"
    env GXV_GX_DEDUP=2 GXV_GX_DEDUP_CLASSES=$cl GXV_GX_DEDUP_DIGEST=1 "$out/e2e-plain" "$frames" "$seed" > "$out/ek2.txt" 2> "$out/ek2.err" || bad "e2e seed $seed CLASSES=$cl =2 run"
    for m in 1 2; do cmp -s <(grep '^frame' "$out/e0.txt") <(grep '^frame' "$out/ek$m.txt") || bad "e2e seed $seed: CLASSES=$cl =$m pictures differ from =0"; done
    bk=$(awk '/^stream/{s+=$4} END{print s+0}' "$out/ek1.txt")
    [ "$bk" -lt "$b0" ] && [ "$bk" -gt "$b1" ] || bad "e2e seed $seed: CLASSES=$cl stream $bk bytes, not between =1's $b1 and =0's $b0"
    set -- $(verify_of "$out/ek2.err")
    [ "${1:-0}" -gt 0 ] && [ "${2:-1}" = 0 ] || bad "e2e seed $seed: CLASSES=$cl =2 verify: ${1:-?} checked, ${2:-?} differing"
    echo "e2e seed $seed: CLASSES=$cl stream $bk bytes; =2 verify ${1:-?} checked, ${2:-?} differing"
  done
done
# (built without the gate: the stream of the gated build with the switch 0, byte for byte, whatever the switches say)
for seed in $seeds; do
  env GXV_GX_DEDUP=0 "$out/e2e-plain" "$frames" "$seed" > "$out/eb0.txt" 2> /dev/null
  env GXV_GX_DEDUP=1 GXV_GX_DEDUP_CENSUS=8 "$out/e2e-off" "$frames" "$seed" > "$out/eb1.txt" 2> "$out/eb1.err" || bad "e2e seed $seed: the build without the gate"
  cmp -s <(grep '^stream' "$out/eb0.txt") <(grep '^stream' "$out/eb1.txt") || bad "e2e seed $seed: the build without the gate writes another stream"
  grep -q "not in this build" "$out/eb1.err" || bad "e2e seed $seed: the build without the gate did not say so"
done
echo "e2e without the gate (PORT_GX_DEDUP_BUILD unset): the stream of =0, byte for byte"
for mu in 1 2; do
  GXDD_E2E_MUTANT=$mu GXV_GX_DEDUP=0 "$out/e2e-plain" "$frames" 1 2> /dev/null | grep '^frame' > "$out/em0.txt"
  GXDD_E2E_MUTANT=$mu GXV_GX_DEDUP=1 "$out/e2e-plain" "$frames" 1 2> /dev/null | grep '^frame' > "$out/em1.txt"
  GXDD_E2E_MUTANT=$mu GXV_GX_DEDUP=2 GXV_GX_DEDUP_DIGEST=1 "$out/e2e-plain" "$frames" 1 > /dev/null 2> "$out/em2.err"
  set -- $(verify_of "$out/em2.err")
  if cmp -s "$out/em0.txt" "$out/em1.txt"; then bad "e2e mutant $mu: =1's pictures did not change"; fi
  [ "${2:-0}" -gt 0 ] || bad "e2e mutant $mu: =2 found nothing differing"
  echo "e2e mutant $mu: caught (=2: ${1:-?} checked, ${2:-?} differing)"
done
# (sanitizers: a subset)
for m in 0 1 2; do
  env FEW_GXDEDUP_RAND=3 FEW_COPY=5 FEW_RETAINED=1 FEW_JPA=100 FEW_BIG=3 FEW_GXDEDUP=$m GXV_GX_DEDUP_DIGEST=1 "$out/few-asan" 12 2 > /dev/null 2> "$out/asan.err" || { bad "fixture ASan/UBSan =$m"; tail -5 "$out/asan.err"; }
  env GXV_GX_DEDUP=$m GXV_GX_DEDUP_CENSUS=4 "$out/e2e-asan" 12 2 > /dev/null 2> "$out/asan2.err" || { bad "e2e ASan/UBSan =$m"; tail -5 "$out/asan2.err"; }
done
echo "fixture runs $runs"
if [ "$fail" != 0 ]; then echo "test_gx_dedup: FAIL"; exit 1; fi
echo "test_gx_dedup: PASS"

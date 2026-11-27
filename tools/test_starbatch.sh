#!/usr/bin/env bash
# GXV_STARBATCH (starbatch25): (1) the pure logic of the planner (tests/starbatch_logic_test.cpp); (2) the production decomp/src/Game/System/DrawBuffer.cpp (a
# drawer: add, light sort, draw, portDrawPort) with the console's GX library, the production vgx front end and the GLES back end on Mesa (tests/starbatch_gl_*.cpp,
# tools/starbatch_build.sh): scenes of 100 .. 256 star-bit-like lit packets (a 40-triangle star prism, six colours by differed lists, light groups, culled
# packets, colours changing between pictures, a second drawer that leaks the colour of the first). Every picture with GXV_STARBATCH=1 (ordered) and =2 (planned and
# checked, drawn in the original order) must be bit-identical with GXV_STARBATCH=0, under the front end's usual switches; with it on the draw records and the
# differed lists sent must fall; refused scenes (blended material, depth write off, differed lists of two skeletons) must draw as before with the reason counted;
# the negative controls (the same pieces added in colour order, no planner) must DIFFER where order shows (blend, depth write off, skeletons, the leaked colour)
# and be identical where it cannot (opaque, depth write, no leak: the commutation the planner stands on).
#   tools/test_starbatch.sh [out-dir]     env: KHR_INCLUDE, SB_OBJ_CACHE (a build dir with gx/ and vgx/ objects to reuse), SB_QUICK=1 (fewer variants)
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
fails=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; fails=$((fails + 1)); }

nice "${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/decomp/libs/JSystem/include" "$root/tests/starbatch_logic_test.cpp" -o "$out/logic" || { echo "FAILED: build of the logic test"; exit 1; }
"$out/logic" > "$out/logic.log"; tail -3 "$out/logic.log"
grep -q FAIL "$out/logic.log" && fail "pure logic" || pass "pure logic"

"$root/tools/starbatch_build.sh" "$out/b" > "$out/build.log" 2>&1 || { tail -20 "$out/build.log"; echo "FAILED: build"; exit 1; }
exe="$out/b/starbatch_gl"
base=(LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0
      GXV_DSTALPHA_ZERO=1 GXV_FIX_DSTA=1 GXV_NODISCARD=1 GXV_FIX_LOGICOP=1 GXV_SOFTXFORM=32 GXV_RIGIDMERGE=1 SB_FRAMES=8)
run() { # run <tag> <env...>   -> $out/<tag>.log
  local tag=$1; shift
  env "${base[@]}" "$@" "$exe" 2>&1 | grep -E "^(frame|sortstats|liststats|FAIL)" > "$out/$tag.log"
  grep -q FAIL "$out/$tag.log" && { echo "run failed: $tag"; return 1; }
  return 0
}
hashes() { grep -E "^frame" "$out/$1.log" | sed 's/ records.*//'; }
recs() { grep -E "^frame" "$out/$1.log" | tail -1 | sed 's/.*records=\([0-9]*\).*/\1/'; }
stat() { grep -E "^sortstats" "$out/$1.log" | sed "s/.* $2 \([0-9]*\).*/\1/"; }
lstat() { grep -E "^liststats" "$out/$1.log" | sed "s/.* $2 \([0-9]*\).*/\1/"; }

# variants of the front end's switches (the console's: SOFTXFORM 32, RIGIDMERGE 1; the others the other values)
variants=("GXV_STATIC_INST=0" "GXV_STATIC_INST=1" "GXV_RIGIDMERGE=0" "GXV_RIGIDMERGE=2 GXV_STATIC_INST=1" "GXV_SOFTXFORM=128" "GXV_J3D_DIFFSKIP=0" "GXV_J3D_MATMERGE=0" "GXV_SPLIT=1" "GXV_DLWELD=1")
[[ "${SB_QUICK:-0}" == 1 ]] && variants=("GXV_STATIC_INST=1" "GXV_RIGIDMERGE=0")
planned=(1 2 3 7 8 9)
refused=(4 5 6)
declare -A why=([4]=matlist [5]=matlist [6]=skeleton)
for sc in "${planned[@]}" "${refused[@]}"; do
  for v in "GXV_STATIC_INST=0" "${variants[@]}"; do
    t="s$sc.$(echo "$v" | tr ' =' '__')"
    run "$t.0" SB_SCENE=$sc GXV_STARBATCH=0 $v || { fail "scene $sc $v reference"; continue; }
    run "$t.1" SB_SCENE=$sc GXV_STARBATCH=1 $v || { fail "scene $sc $v mode 1"; continue; }
    run "$t.2" SB_SCENE=$sc GXV_STARBATCH=2 $v || { fail "scene $sc $v mode 2"; continue; }
    if [[ "$(hashes $t.0)" == "$(hashes $t.1)" && -n "$(hashes $t.0)" ]]; then ok1=1; else ok1=0; fi
    if [[ "$(hashes $t.0)" == "$(hashes $t.2)" ]]; then ok2=1; else ok2=0; fi
    r0=$(recs $t.0); r1=$(recs $t.1); r2=$(recs $t.2)
    d0=$(lstat $t.0 diffSent); d1=$(lstat $t.1 diffSent)
    bad=$(stat $t.1 bad)
    vbad=$(stat $t.2 vbad); vlist=$(stat $t.2 vlist); vinst=$(stat $t.2 vinst)
    line="scene $sc [$v]: records $r0 -> $r1 (mode 2: $r2), differed lists sent $d0 -> $d1"
    # (starbatch26: mode 2 sends the planned order a second time into the same recording: more records than mode 0 where a drawer was planned, the same picture; the
    # per-packet comparison of the two passes must have compared packets in the planned scenes and found 0 differences)
    vok=1; [[ "$vbad" != 0 || "$vlist" != 0 ]] && vok=0
    if [[ " ${planned[*]} " == *" $sc "* && "${vinst:-0}" -lt 10 ]]; then vok=0; fi
    if [[ $ok1 == 1 && $ok2 == 1 && "$bad" == 0 && $vok == 1 && "$r2" -ge "$r0" ]]; then
      if [[ " ${planned[*]} " == *" $sc "* ]]; then
        # (a front end that cannot join runs: RIGIDMERGE=0 or a draw that is CPU-transformed already joins by itself: the records may be equal; the lists are not)
        if [[ $d1 -lt $d0 ]]; then pass "$line, pictures identical"; else fail "$line: no list saved"; fi
        if [[ "$v" == "GXV_STATIC_INST=0" && $r1 -ge $r0 ]]; then fail "$line: no record saved"; fi
      else
        [[ "$r1" == "$r0" && "$d1" == "$d0" && "$(grep -c "${why[$sc]}=" "$out/$t.1.log")" -ge 1 ]] && pass "$line, left alone (${why[$sc]}), pictures identical" || fail "$line: not left alone as ${why[$sc]}"
      fi
    else
      fail "$line: pictures differ (mode 1 $ok1, mode 2 $ok2) or plan checks failed ($bad) or mode 2 compared packets that differ (vbad $vbad vlist $vlist vinst $vinst)"
    fi
  done
done

# ---- the packet limit: 257 packets are not planned (the stretch is left as it is)
run big.0 SB_SCENE=8 SB_BIG=257 GXV_STARBATCH=0; run big.1 SB_SCENE=8 SB_BIG=257 GXV_STARBATCH=1
if [[ "$(hashes big.0)" == "$(hashes big.1)" && "$(grep -c "many=" "$out/big.1.log")" -ge 1 && "$(recs big.0)" == "$(recs big.1)" ]]; then pass "257 packets: left alone (many), identical"; else fail "257 packets"; fi
# ---- few packets (GXV_STARBATCH_MIN)
run few.0 SB_SCENE=1 SB_N=3 GXV_STARBATCH=0; run few.1 SB_SCENE=1 SB_N=3 GXV_STARBATCH=1
if [[ "$(hashes few.0)" == "$(hashes few.1)" && "$(grep -c "few=" "$out/few.1.log")" -ge 1 ]]; then pass "3 packets: left alone (few), identical"; else fail "3 packets"; fi
# ---- other seeds
for seed in 1 2 3 4 5; do
  run "seed$seed.0" SB_SCENE=2 SB_SEED=$seed GXV_STARBATCH=0 GXV_STATIC_INST=1; run "seed$seed.1" SB_SCENE=2 SB_SEED=$seed GXV_STARBATCH=1 GXV_STATIC_INST=1
  [[ "$(hashes seed$seed.0)" == "$(hashes seed$seed.1)" ]] && pass "seed $seed (two light groups): identical" || fail "seed $seed"
done

# ---- negative controls: the same pieces added in colour order, no planner
for sc in 1 4 5 6; do
  run nv$sc.a SB_SCENE=$sc SB_NOLEAK=1 GXV_STARBATCH=0 SB_NAIVE=0; run nv$sc.b SB_SCENE=$sc SB_NOLEAK=1 GXV_STARBATCH=0 SB_NAIVE=1
  h0=$(hashes nv$sc.a | tail -1); h1=$(hashes nv$sc.b | tail -1)
  if [[ $sc == 1 ]]; then
    [[ "$h0" == "$h1" ]] && pass "control, scene 1 (opaque, depth write, no leak): colour order draws the same picture (the commutation)" || fail "control scene 1 differs: the commutation does not hold"
  else
    [[ "$h0" != "$h1" ]] && pass "control, scene $sc: a changed order SHOWS in the picture (the planner must refuse it)" || fail "control scene $sc: the picture does not show the order"
  fi
done
run lk.a SB_SCENE=1 GXV_STARBATCH=0 SB_NAIVE=0; run lk.b SB_SCENE=1 GXV_STARBATCH=0 SB_NAIVE=1
[[ "$(hashes lk.a | tail -1)" != "$(hashes lk.b | tail -1)" ]] && pass "control, the leaking drawer: another last packet SHOWS (the planner keeps the last packet last)" || fail "control leak: not seen"

# ---- one line of numbers for the report (scene 1: 110 star bits + 12 others)
run n.0 SB_SCENE=1 GXV_STARBATCH=0 GXV_STATIC_INST=1 SB_FRAMES=24; run n.1 SB_SCENE=1 GXV_STARBATCH=1 GXV_STATIC_INST=1 SB_FRAMES=24
fe() { grep -E "^frame" "$out/$1.log" | tail -12 | sed 's/.*fe_ms=//' | sort -n | sed -n '6p'; }
echo "numbers (scene 1, 110 star bits, GXV_STATIC_INST=1): draw records $(recs n.0) -> $(recs n.1); differed lists sent a picture $(( $(lstat n.0 diffSent) / 24 )) -> $(( $(lstat n.1 diffSent) / 24 )); front end median ms (this machine, 12 last pictures) $(fe n.0) -> $(fe n.1)"
[[ $fails == 0 ]] && echo "starbatch: all checks passed" && exit 0
echo "starbatch: $fails FAILED"; exit 1

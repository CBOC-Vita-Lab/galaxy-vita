#!/usr/bin/env bash
# GXV_LIGHTVOL (lightvol25): the pure logic (tests/lightvol_test.cpp: box projection never culls a visible box, grouping disjoint, the plan's fades monotone and
# jitter-stable, negative controls) and the production VolumeModelDrawer.cpp through the console's GX library, the vgx front end and the GLES back end on Mesa
# (tests/lightvol_gl_{game,main}.cpp, tools/lightvol_build.sh, tools/lightvol_cmp.py). No game data.
#   tools/test_lightvol.sh [out-dir]    env: KHR_INCLUDE, LV_KEEP_BUILD=1 (keep the build objects), LV_PURE_ONLY=1
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
fails=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; fails=$((fails + 1)); }

# ---- the pure logic
nice "${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/platform/include" "$root/tests/lightvol_test.cpp" -o "$out/pure" || { echo "FAILED: build of the pure test"; exit 1; }
"$out/pure" > "$out/pure.log"; tail -3 "$out/pure.log"
grep -q FAIL "$out/pure.log" && fail "pure logic" || pass "pure logic"
[[ "${LV_PURE_ONLY:-0}" == 1 ]] && { [[ $fails == 0 ]] && echo "lightvol: all checks passed" && exit 0; exit 1; }

# ---- the pictures
"$root/tools/lightvol_build.sh" "$out/b" > "$out/build.log" 2>&1 || { tail -20 "$out/build.log"; echo "FAILED: build"; exit 1; }
export LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0
# (the console's defaults that the picture depends on: entry.cpp)
export GXV_DSTALPHA_ZERO=1 GXV_FIX_DSTA=1 GXV_NODISCARD=1 GXV_FIX_LOGICOP=1
exe="$out/b/lightvol_gl"
cmp() { python3 -I "$root/tools/lightvol_cmp.py" "$@"; }
run() { # run <dir> <scene> <frames> [env...]
  local d=$1 sc=$2 fr=$3; shift 3
  rm -rf "$d"; mkdir -p "$d"
  env LV_SCENE=$sc LV_FRAMES=$fr LV_DUMPDIR="$d" "$@" "$exe" 2>&1 | grep -E "^(frame|FAIL|log: lightvol)" > "$d.log"
  grep -q FAIL "$d.log" && echo "run failed: $d" && return 1
  return 0
}
draws() { grep -E "^frame$2 " "$1.log" | sed 's/.*draws=//'; }
for sc in 1 2 3 4 5; do
  run "$out/s$sc.m0" $sc 2 GXV_LIGHTVOL=0 || fail "scene $sc reference"
  for m in 1 2 3 8; do
    run "$out/s$sc.m$m" $sc 2 GXV_LIGHTVOL=$m
    r=$(cmp "$out/s$sc.m0" "$out/s$sc.m$m")
    if [[ $(echo "$r" | grep -c IDENTICAL) == 2 ]]; then pass "scene $sc mode $m bit-identical to mode 0 ($(draws "$out/s$sc.m0" 2) -> $(draws "$out/s$sc.m$m" 2) draws)"; else fail "scene $sc mode $m differs: $r"; fi
  done
done
# the exact options must actually cut draws (a test that passes by doing nothing is no test)
d0=$(draws "$out/s2.m0" 2); d1=$(draws "$out/s2.m1" 2); d2=$(draws "$out/s1.m0" 2); d3=$(draws "$out/s1.m2" 2)
[[ $d1 -lt $d0 ]] && pass "cull removes draws (scene 2: $d0 -> $d1)" || fail "cull removed nothing ($d0 -> $d1)"
[[ $d3 -lt $d2 ]] && pass "pass-major groups merge draws (scene 1: $d2 -> $d3)" || fail "grouping merged nothing ($d2 -> $d3)"

# ---- negative controls: the pictures must SEE a wrong grouping and a wrong cut
for sc in 1 3; do
  run "$out/n1.s$sc" $sc 2 LV_NEG=1 GXV_LIGHTVOL=0
  r=$(cmp "$out/s$sc.m0" "$out/n1.s$sc"); [[ $(echo "$r" | grep -c DIFF) -ge 1 ]] && pass "NEG1 scene $sc: one group for all volumes is a different picture ($(echo "$r" | tail -1 | sed 's/frame f0.: //'))" || fail "NEG1 scene $sc not seen"
done
run "$out/n2.s2" 2 2 LV_NEG=2 GXV_LIGHTVOL=0
r=$(cmp "$out/s2.m0" "$out/n2.s2"); [[ $(echo "$r" | grep -c DIFF) -ge 1 ]] && pass "NEG2 scene 2: culling by the box centre is a different picture ($(echo "$r" | tail -1 | sed 's/frame f0.: //'))" || fail "NEG2 not seen"
run "$out/n3.s1" 1 2 LV_NEG=1 LV_PASSES=9 GXV_LIGHTVOL=0
r=$(cmp "$out/s1.m0" "$out/n3.s1"); [[ $(echo "$r" | grep -c DIFF) -ge 1 ]] && pass "NEG3 scene 1: without the back/front passes the picture is a different one (the tint IS the passes)" || fail "NEG3 not seen"
run "$out/n4.s1" 1 2 GXV_LIGHTVOL=0 LV_FLIP=1
r=$(cmp "$out/s1.m0" "$out/n4.s1"); [[ $(echo "$r" | grep -c DIFF) -ge 1 ]] && pass "NEG4 scene 1: front and back faces swapped is a different picture (the picture depends on the winding)" || fail "NEG4 not seen"

# ---- the LOD (inexact): a walk past the volumes; the picture's distance to the original, and the step between consecutive pictures
walk=(LV_MOVE=0,0,-60 GXV_LIGHTVOL_KEEP=10 GXV_LIGHTVOL_PX=6000 GXV_LIGHTVOL_NEAR=300)
run "$out/w.ref" 1 40 GXV_LIGHTVOL=0 "${walk[@]}"
run "$out/w.lod" 1 40 GXV_LIGHTVOL=7 "${walk[@]}"
cmp "$out/w.ref" "$out/w.lod" | awk '{print $2, $4, $5}' | tr '\n' ' ' | fold -w 150 > "$out/w.psnr.txt"
echo "LOD walk (scene 1, 40 pictures, KEEP 10, PX 6000, NEAR 300), differing px / rgb psnr per picture vs the original:"; cat "$out/w.psnr.txt"; echo
cmp --steps "$out/w.lod" > "$out/w.steps.txt"
max=$(awk 'NR>1 {split($4,a,"="); if (a[2]+0>m) m=a[2]+0} END{print m+0}' "$out/w.steps.txt")   # (the first step is the harness's own first picture)
echo "largest step between consecutive LOD pictures (after the first): $max / 255"
[[ $max -le 40 ]] && pass "LOD: no pop (largest step $max of 255 per channel, the fade's slew)" || fail "LOD: a step of $max"
# (the first step is the harness's own first picture; the pop control for the plan is in the pure test: NEG the naive choice does pop)
run "$out/c.full" 5 2 GXV_LIGHTVOL=0
run "$out/c.cut" 5 2 GXV_LIGHTVOL=7 GXV_LIGHTVOL_KEEP=6 GXV_LIGHTVOL_NEAR=100 GXV_LIGHTVOL_PX=6000
r=$(cmp "$out/c.full" "$out/c.cut" | tail -1); echo "scene 5 (floor alpha 255), KEEP 6: $r"
run "$out/c.noclr" 5 2 GXV_LIGHTVOL=7 GXV_LIGHTVOL_KEEP=6 GXV_LIGHTVOL_NEAR=100 GXV_LIGHTVOL_PX=6000 GXV_LIGHTVOL_CLEAR=0
r1=$(cmp "$out/c.full" "$out/c.cut" | tail -1 | sed 's/.*a_psnr=\([^ ]*\).*/\1/'); r2=$(cmp "$out/c.full" "$out/c.noclr" | tail -1 | sed 's/.*a_psnr=\([^ ]*\).*/\1/')
echo "alpha plane vs the original: CLEAR=1 a_psnr $r1, CLEAR=0 a_psnr $r2"
[[ "$r1" == inf && "$r2" != inf ]] && pass "a cut volume keeps its alpha clear (CLEAR=1: alpha identical; CLEAR=0 control: alpha differs)" || fail "alpha clear control (CLEAR=1 $r1, CLEAR=0 $r2)"
# the census
for sc in 1 2; do run "$out/cs$sc" $sc 10 GXV_LIGHTVOL=8 GXV_LIGHTVOL_CENSUS=10 GXV_LIGHTVOL_KEEP=10 GXV_LIGHTVOL_NEAR=300 GXV_LIGHTVOL_PX=6000; grep "log: lightvol" "$out/cs$sc.log" | head -2; done
grep -q "log: lightvol: 10 pictures" "$out/cs1.log" && pass "census line" || fail "census line"
# default path: mode 0 draws exactly the original's number of draws (4 per volume + the world)
[[ $(draws "$out/s1.m0" 2) == 81 ]] && pass "mode 0: 4 draws per volume (81 with the world)" || fail "mode 0 draw count"
[[ "${LV_KEEP_BUILD:-0}" == 1 ]] || rm -rf "$out/b"
echo "pictures in $out"
[[ $fails == 0 ]] && { echo "lightvol: all checks passed"; exit 0; } || { echo "lightvol: $fails FAILED"; exit 1; }

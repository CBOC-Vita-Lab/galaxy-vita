#!/usr/bin/env bash
# GXV_HSR_ORDER (hsrorder25): the planner against a software model of the GPU's per-pixel rules (tests/vgx_hsrorder_plan.cpp: random pictures, negative
# controls for every rule) and the real front end + GLES back end on Mesa (tests/vgx_hsrorder_gl.cpp: synthetic pictures with opaque, alpha-tested, blended and
# no-depth-write draws and coplanar decals). Needs CPLUS_INCLUDE_PATH=<tree>/platform/include (as tools/test_vgx_indcap_gl.sh).
#   tools/test_vgx_hsrorder_gl.sh [output-dir]       (Khronos headers: KHR_INCLUDE, default /work/khr/include)
#   HSR_BASE_TREE=<clean export of the base commit>: the same test is built against it (-DHSR_BASE is not needed: it uses no new API except DebugHsr, stubbed) and its pictures
#   must equal this tree's with the switches off ("default path bit-identical"); otherwise the baseline hashes in HSR_BASE_HASHES (a file of frame lines) are used.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$root/platform/vgx/src/$unit.cpp"); done
[[ "${HSR_SKIP_BUILD:-0}" == 1 ]] || {
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -Wall -I"$root/platform/vgx/src" -I"$root/platform/include" "$root/tests/vgx_hsrorder_plan.cpp" -o "$out/plan"
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
    "$root/tests/vgx_hsrorder_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/hsr"
}
"$out/plan" | tee "$out/plan.log" | grep -v "^PASS: [a-z0-9=]" | tail -3
grep -q FAIL "$out/plan.log" && { echo "FAILED: planner model test"; exit 1; }
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0"
run() { # tag, env...
  local tag=$1; shift
  env -u GXV_HSR_ORDER -u GXV_HSR_ORDER_TIES -u GXV_DROP_FS -u GXV_DROP_CLASS $E "$@" "$out/hsr" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL" "$out/$tag.log" && { echo "FAILED: $tag"; grep FAIL "$out/$tag.log" | head; exit 1; }
  grep -q '^frame1.0' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^frame' "$out/$tag.log" | sed 's/ scenes=[0-9]*//' > "$out/$tag.hash"
}
run off
run off0 GXV_HSR_ORDER=0
run on1 GXV_HSR_ORDER=1
run on3 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=3
run ties0 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=0
run alt GXV_HSR_ORDER=3
run census GXV_HSR_ORDER=2
run dropT GXV_DROP_CLASS=T
run dropB GXV_DROP_CLASS=B
run dropFs GXV_DROP_FS=0
# (dread25) the mode set by a stage preset (port::stageknob) instead of the environment: the same pictures, the same reordering; set late (after round 0), round 0 is not reordered
run stagek HSR_TEST_STAGEKNOB=1:1
run stagelate HSR_TEST_STAGEKNOB=1:1 HSR_TEST_STAGEKNOB_LATE=1
run stageoff HSR_TEST_STAGEKNOB=0
info() { grep '^info hsr' "$out/$1.log"; }
echo "off:    $(info off)"; echo "on1:    $(info on1)"; echo "on3:    $(info on3)"; echo "ties0:  $(info ties0)"; echo "alt:    $(info alt)"; echo "census: $(info census)"; echo "dropT:  $(info dropT)"
cmp -s "$out/off.hash" "$out/off0.hash" || { echo "FAILED: GXV_HSR_ORDER=0 differs from unset"; exit 1; }
cmp -s "$out/off.hash" "$out/census.hash" || { echo "FAILED: the census changed a picture"; exit 1; }
cmp -s "$out/off.hash" "$out/on1.hash" || { echo "FAILED: GXV_HSR_ORDER=1 (ties 1) changed a picture"; diff "$out/off.hash" "$out/on1.hash"; exit 1; }
cmp -s "$out/off.hash" "$out/on3.hash" || { echo "FAILED: GXV_HSR_ORDER=1 (ties 3) changed a picture"; diff "$out/off.hash" "$out/on3.hash"; exit 1; }
cmp -s "$out/off.hash" "$out/alt.hash" || { echo "FAILED: GXV_HSR_ORDER=3 (alternating) changed a picture"; diff "$out/off.hash" "$out/alt.hash"; exit 1; }
# the reorder really happened, and the negative control really differs
tm=$(info on1 | sed -E 's/.*tmoved=([0-9]+).*/\1/'); [[ $tm -gt 0 ]] || { echo "FAILED: nothing was reordered (tmoved=$tm)"; exit 1; }
if cmp -s "$out/off.hash" "$out/ties0.hash"; then echo "FAILED: NEGATIVE CONTROL: ties 0 should change the tie frames (2 and 4) and does not"; exit 1; fi
{ diff "$out/off.hash" "$out/ties0.hash" || true; } | grep '^>' | sed -E 's/^> (frame[0-9]+).*/\1/' | tr '\n' ' ' > "$out/ties0.frames" || true
echo "NEGATIVE CONTROL: ties 0 changes: $(cat "$out/ties0.frames")"
grep -q "frame2" "$out/ties0.frames" || { echo "FAILED: ties 0 should change frame 2"; exit 1; }
cmp -s "$out/off.hash" "$out/stagek.hash" || { echo "FAILED: the stage preset's hsr_order=1 changed a picture"; diff "$out/off.hash" "$out/stagek.hash"; exit 1; }
cmp -s "$out/off.hash" "$out/stagelate.hash" || { echo "FAILED: a late stage preset changed a picture"; exit 1; }
cmp -s "$out/off.hash" "$out/stageoff.hash" || { echo "FAILED: the stage preset's hsr_order=0 changed a picture"; exit 1; }
tk=$(info stagek | sed -E 's/.*tmoved=([0-9]+).*/\1/'); tl=$(info stagelate | sed -E 's/.*tmoved=([0-9]+).*/\1/'); to=$(info stageoff | sed -E 's/.*tmoved=([0-9]+).*/\1/')
[[ $tk -eq $tm ]] || { echo "FAILED: the stage preset reordered $tk draws, the environment $tm"; exit 1; }
[[ $tl -gt 0 && $tl -lt $tm ]] || { echo "FAILED: a preset set after round 0 should reorder only round 1 (got $tl of $tm)"; exit 1; }
[[ $to -eq 0 ]] || { echo "FAILED: hsr_order=0 by a preset reordered $to"; exit 1; }
echo "stage preset: hsr_order=1 reordered $tk (= the environment's $tm), set late $tl, hsr_order=0 $to"
for tag in dropT dropB; do cmp -s "$out/off.hash" "$out/$tag.hash" && { echo "FAILED: GXV_DROP_CLASS ($tag) changed nothing"; exit 1; }; done
echo "PASS: planner model test; picture hashes of GXV_HSR_ORDER=0/2 (census), 1 (ties 1, 3), 3 (alternating) equal the original order's for all frames; reordered $tm alpha-tested draws; ties 0 (a rule disabled) differs; drop tests change the pictures"

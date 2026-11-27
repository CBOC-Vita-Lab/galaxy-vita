#!/usr/bin/env bash
# GXV_STATIC_INST (consecutive uses of one cached list's static run that differ only in their model-view matrix drawn as one record, from a
# K-times replicated copy of the run with the GXV_RIGIDMERGE matrix palette; vgx_draw.cpp "static instances", vgx_dlcache.cpp "instance
# buffers"): the production GX front end and the production GLES back end on Mesa (tests/vgx_dllearn_gl.cpp: rigid objects drawn from
# display lists in guest RAM, runs of the same list with the same material, frame after frame). Every frame's picture must be bit-identical
# with the switch off (unset, 0) and on (1, 3), under GXV_SPLIT 0/1/2 (2 also compares the finishing thread's records with the player's),
# GXV_RSCACHE 0/1/2, GXV_SOFTXFORM 128/0, GXV_RIGIDMERGE 1/2, GXV_DLWELD 0/1, GXV_ABSIDX 0/1, GXV_FE_PARALLEL 1; with it on the draw records
# must be fewer than with it off (and with GXV_STATIC_INST_KB=0 or GXV_STATIC_INST_VERTS=3, or GXV_RIGIDMERGE=0, the same as off).
#   tools/test_vgx_staticinst_gl.sh [output-dir]       (no game data, no Vita; CPLUS_INCLUDE_PATH must find EGL/GLES2 headers)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_STATICINST_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_dllearn_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_staticinst_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_STATIC_INST -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_SOFTXFORM -u GXV_DLWELD -u GXV_ABSIDX \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_staticinst_gl" 40 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH|mismatching [1-9]" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH|mismatching" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
  grep -E '^staticinst' "$out/$tag.log" | sed 's/.* joins \([0-9]*\) .*/\1/' > "$out/$tag.joins"
}
n=0
for rm in 1 2; do
  for split in 0 1 2; do
    for rs in 0 1 2; do
      for soft in 128 0; do
        for weld in 0 1; do
          for abs in 0 1; do
            # (a sample of the product: every value of every switch, each with the others at their usual values)
            others=$(( (rs != 1) + (soft != 128) + (weld != 1) + (abs != 1) ))
            [[ $others -le 1 ]] || continue
            base=(GXV_RIGIDMERGE=$rm GXV_SPLIT=$split GXV_RSCACHE=$rs GXV_SOFTXFORM=$soft GXV_DLWELD=$weld GXV_ABSIDX=$abs)
            t="rm$rm-split$split-rs$rs-soft$soft-weld$weld-abs$abs"
            run "$t-unset" "${base[@]}"
            run "$t-0" "${base[@]}" GXV_STATIC_INST=0
            run "$t-1" "${base[@]}" GXV_STATIC_INST=1
            run "$t-3" "${base[@]}" GXV_STATIC_INST=3
            off=$(cat "$out/$t-unset.records")
            [[ $(cat "$out/$t-0.records") == "$off" ]] || { echo "FAIL: $t GXV_STATIC_INST=0 changed the records"; exit 1; }
            for v in 1 3; do
              on=$(cat "$out/$t-$v.records")
              [[ $on -lt $off && $(cat "$out/$t-$v.joins") -gt 0 ]] || { echo "FAIL: $t GXV_STATIC_INST=$v: $on records (off $off), joins $(cat "$out/$t-$v.joins")"; exit 1; }
            done
            echo "  $t: records off $off, on $(cat "$out/$t-1.records")"
            n=$((n + 4))
          done
        done
      done
    done
  done
done
# limits: no budget, no list small enough, the palette switch off: as off
run kb0 GXV_RIGIDMERGE=1 GXV_STATIC_INST=1 GXV_STATIC_INST_KB=0
run verts3 GXV_RIGIDMERGE=1 GXV_STATIC_INST=1 GXV_STATIC_INST_VERTS=3
run rm0 GXV_RIGIDMERGE=0 GXV_STATIC_INST=1
run rm0-off GXV_RIGIDMERGE=0
run fepar GXV_RIGIDMERGE=1 GXV_STATIC_INST=1 GXV_FE_PARALLEL=1 GXV_SPLIT=1
run verts150 GXV_RIGIDMERGE=1 GXV_STATIC_INST=1 GXV_STATIC_INST_VERTS=150
run vary-off GXV_RIGIDMERGE=1 SI_VARY=1
run vary-on GXV_RIGIDMERGE=1 SI_VARY=1 GXV_STATIC_INST=1
run vary-on-split2 GXV_RIGIDMERGE=1 SI_VARY=1 GXV_STATIC_INST=1 GXV_SPLIT=2
run vary-on-weld GXV_RIGIDMERGE=1 SI_VARY=1 GXV_STATIC_INST=1 GXV_DLWELD=1 GXV_ABSIDX=0
n=$((n + 6))
for t in vary-on vary-on-split2 vary-on-weld; do
  [[ "$(cat "$out/$t.hash")" == "$(cat "$out/vary-off.hash")" ]] || { echo "FAIL: $t picture differs from vary-off"; exit 1; }
  [[ $(cat "$out/$t.joins") -gt 0 ]] || { echo "FAIL: $t joined nothing"; exit 1; }
  mv "$out/$t.hash" "$out/$t.vhash"
done
mv "$out/vary-off.hash" "$out/vary-off.vhash"
echo "  varying scene: records off $(cat "$out/vary-off.records"), on $(cat "$out/vary-on.records") (last frame); $(grep '^staticinst' "$out/vary-on.log")"
off=$(cat "$out/rm1-split0-rs1-soft128-weld1-abs1-unset.records")
for t in kb0 verts3; do
  [[ $(cat "$out/$t.records") == "$off" && $(cat "$out/$t.joins") == 0 ]] || { echo "FAIL: $t: $(cat "$out/$t.records") records (off $off), joins $(cat "$out/$t.joins")"; exit 1; }
done
[[ $(cat "$out/rm0.records") == $(cat "$out/rm0-off.records") && $(cat "$out/rm0.joins") == 0 ]] || { echo "FAIL: GXV_RIGIDMERGE=0 with GXV_STATIC_INST=1 changed something"; exit 1; }
v150=$(cat "$out/verts150.records")
[[ $v150 -lt $off && $v150 -gt $(cat "$out/rm1-split0-rs1-soft128-weld1-abs1-1.records") ]] || { echo "FAIL: GXV_STATIC_INST_VERTS=150: $v150 records"; exit 1; }
ref=$(cat "$out/rm1-split0-rs1-soft128-weld1-abs1-unset.hash")
for f in "$out"/*.hash; do
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "PASS: pictures identical across $n runs ($ref); records with GXV_STATIC_INST=1 fewer in every setting"

#!/usr/bin/env bash
# GXV_DL_DEDUP (bowl32): display lists whose decoded geometry is equal share one static buffer (and, with GXV_STATIC_INST, one instance buffer):
# the production GX front end and the production GLES back end on Mesa (tests/vgx_dllearn_gl.cpp with DD_COPIES=n: the model's lists exist n times at
# other addresses, with the same bytes). Every picture must be bit-identical for GXV_DL_DEDUP 0 / 1 / 2 (and unset), with GXV_STATIC_INST 0 / 1 / 3,
# GXV_SPLIT 0/1/2, GXV_DLWELD 0/1, GXV_ABSIDX 0/1, GXV_RIGIDMERGE 1/2, and with the copies' arrays changed (SI_VARY=1: a vertex of the first copy moves, its write
# notified: that list builds again and must stop sharing). With the switch on and copies > 1, buffers must be shared (hits > 0); with STATIC_INST on, the records
# must be fewer than with dedupe off (the entries' runs join); mode 2 shares nothing (hits 0, would-hits > 0, records as off).
#   tools/test_vgx_dldedup_gl.sh [output-dir]       (no game data, no Vita; CPLUS_INCLUDE_PATH must find EGL/GLES2 headers)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_DEDUP_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_dllearn_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_dedup_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_STATIC_INST -u GXV_DL_DEDUP -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_SOFTXFORM -u GXV_DLWELD -u GXV_ABSIDX -u DD_COPIES -u SI_VARY \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_dedup_gl" 40 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
  grep -E '^dedup' "$out/$tag.log" | sed 's/^dedup //' > "$out/$tag.dd"
}
ddget() { sed -nE "s/^(.* )?$2 ([0-9]+).*$/\2/p" "$out/$1.dd" | head -1; } # <tag> <field>
n=0
for rm in 1 2; do
  for split in 0 1 2; do
    for weld in 0 1; do
      for abs in 0 1; do
        [[ $(( (weld != 1) + (abs != 1) + (rm != 1) + (split != 0) )) -le 1 ]] || continue
        base=(GXV_RIGIDMERGE=$rm GXV_SPLIT=$split GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_DLWELD=$weld GXV_ABSIDX=$abs DD_COPIES=3)
        t="rm$rm-split$split-weld$weld-abs$abs"
        run "$t-unset" "${base[@]}"
        run "$t-d0" "${base[@]}" GXV_DL_DEDUP=0
        run "$t-d1" "${base[@]}" GXV_DL_DEDUP=1
        run "$t-d2" "${base[@]}" GXV_DL_DEDUP=2
        run "$t-d1i1" "${base[@]}" GXV_DL_DEDUP=1 GXV_STATIC_INST=1
        run "$t-d0i1" "${base[@]}" GXV_DL_DEDUP=0 GXV_STATIC_INST=1
        run "$t-d1i3" "${base[@]}" GXV_DL_DEDUP=1 GXV_STATIC_INST=3
        [[ $(ddget "$t-d1" hits) -gt 0 ]] || { echo "FAIL: $t DEDUP=1 shared nothing: $(cat "$out/$t-d1.dd")"; exit 1; }
        [[ $(ddget "$t-d2" hits) == 0 && $(ddget "$t-d2" wouldhits) -gt 0 ]] || { echo "FAIL: $t DEDUP=2: $(cat "$out/$t-d2.dd")"; exit 1; }
        [[ $(ddget "$t-d0" hits) == 0 && $(ddget "$t-d0" made) == 0 ]] || { echo "FAIL: $t DEDUP=0 did something: $(cat "$out/$t-d0.dd")"; exit 1; }
        [[ $(cat "$out/$t-d0.records") == $(cat "$out/$t-unset.records") && $(cat "$out/$t-d2.records") == $(cat "$out/$t-unset.records") ]] || { echo "FAIL: $t records differ with dedupe off / census"; exit 1; }
        a=$(cat "$out/$t-d1i1.records"); b=$(cat "$out/$t-d0i1.records")
        [[ $a -lt $b ]] || { echo "FAIL: $t records with dedupe + STATIC_INST $a, with STATIC_INST alone $b"; exit 1; }
        [[ $(ddget "$t-d1i1" instshared) -gt 0 ]] || { echo "FAIL: $t no instance buffer shared: $(cat "$out/$t-d1i1.dd")"; exit 1; }
        echo "  $t: records off $(cat "$out/$t-unset.records"), STATIC_INST $b, dedupe + STATIC_INST $a; $(cat "$out/$t-d1i1.dd")"
        n=$((n + 7))
      done
    done
  done
done
# the copies' arrays change: the changed list builds again, stops sharing, the pictures stay equal across the switch
run vary-d0 DD_COPIES=3 SI_VARY=1 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_STATIC_INST=1
run vary-d1 DD_COPIES=3 SI_VARY=1 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_STATIC_INST=1 GXV_DL_DEDUP=1
run vary-d1-split2 DD_COPIES=3 SI_VARY=1 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_STATIC_INST=1 GXV_DL_DEDUP=1 GXV_SPLIT=2
run vary-d1-noinst DD_COPIES=3 SI_VARY=1 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_DL_DEDUP=1
n=$((n + 4))
for t in vary-d1 vary-d1-split2 vary-d1-noinst; do
  [[ "$(cat "$out/$t.hash")" == "$(cat "$out/vary-d0.hash")" ]] || { echo "FAIL: $t picture differs from vary-d0"; exit 1; }
  mv "$out/$t.hash" "$out/$t.vhash"
done
mv "$out/vary-d0.hash" "$out/vary-d0.vhash"
echo "  varying copies: $(cat "$out/vary-d1.dd")"
# one copy only (DD_COPIES unset): dedupe has nothing to share, nothing changes
run one-d0 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_STATIC_INST=1
run one-d1 GXV_RIGIDMERGE=1 GXV_RSCACHE=1 GXV_SOFTXFORM=128 GXV_STATIC_INST=1 GXV_DL_DEDUP=1
[[ $(cat "$out/one-d0.records") == $(cat "$out/one-d1.records") ]] || { echo "FAIL: one copy: records differ"; exit 1; }
n=$((n + 2))
# every picture of the copy-less runs and of the copy runs must be one picture (the copies are the same mesh, the objects the same): all hashes equal except where the
# test's own scene differs: the copy runs draw the same objects through other lists, so they must equal the copy-less run too
ref=$(cat "$out/one-d0.hash")
for f in "$out"/*.hash; do
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "PASS: pictures identical across $n runs ($ref); buffers shared, records fewer with GXV_DL_DEDUP=1 + GXV_STATIC_INST=1 in every setting"

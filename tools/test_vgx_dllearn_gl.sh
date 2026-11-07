#!/usr/bin/env bash
# GXV_RIGIDMERGE_DLMAX=<n> and GXV_RIGIDMERGE_DLMAX=learn / GXV_RIGIDMERGE_DLLEARN (cached display lists copied into the frame's arena to join palette runs;
# learned: only where that joins a run): the production GX front end and the production GLES back end on Mesa (tests/vgx_dllearn_gl.cpp). Rigid objects are drawn
# from display lists in guest RAM, frame after frame; the pictures of every frame must be bit-identical across the switch values (off, 256 / 512 / 1024
# vertices always copied, learned with caps 256 / 512 / 1024) and the front-end switches (GXV_SPLIT 0/1/2 (2 also compares the join logs of the two
# paths), GXV_RSCACHE 0/1/2, GXV_SOFTXFORM 128/0, GXV_RIGIDMERGE 1/2); the draw records of the last frame are those of the same cap with every list copied;
# the bytes copied are fewer than with every list copied.
#   tools/test_vgx_dllearn_gl.sh [output-dir]       (no game data, no Vita)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_DLLEARN_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_dllearn_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_dllearn_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_RIGIDMERGE -u GXV_RIGIDMERGE_DLMAX -u GXV_RIGIDMERGE_DLLEARN -u GXV_RIGIDMERGE_DLLEARN_MAX -u GXV_SPLIT -u GXV_RSCACHE -u GXV_UNIPACK -u GXV_SOFTXFORM \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_dllearn_gl" 40 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
  grep -E '^copybytes' "$out/$tag.log" | sed 's/copybytes //' > "$out/$tag.bytes"
}
for soft in 128 0; do
  for split in 0 1 2; do
    for rs in 0 1 2; do
      for rm in 1 2; do
        base=(GXV_RIGIDMERGE=$rm GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_RSCACHE=$rs)
        t="soft$soft-split$split-rs$rs-rm$rm"
        [[ $rm == 2 && ( $rs != 1 || $soft != 128 ) ]] && continue
        [[ $soft == 0 && $rs != 1 ]] && continue
        run "$t-off" "${base[@]}"
        for n in 256 512 1024; do
          run "$t-dlmax$n" "${base[@]}" GXV_RIGIDMERGE_DLMAX=$n
          run "$t-learn$n" "${base[@]}" GXV_RIGIDMERGE_DLMAX=learn GXV_RIGIDMERGE_DLLEARN_MAX=$n
        done
        run "$t-learn1" "${base[@]}" GXV_RIGIDMERGE_DLLEARN=1
        run "$t-learn300" "${base[@]}" GXV_RIGIDMERGE_DLLEARN=300
      done
    done
  done
done
# the master switch off: the learn switches do nothing
run rm0-learn GXV_RIGIDMERGE=0 GXV_RIGIDMERGE_DLMAX=learn
run rm0-learn1 GXV_RIGIDMERGE=0 GXV_RIGIDMERGE_DLLEARN=1
run rm0-off GXV_RIGIDMERGE=0
ref=$(cat "$out/soft128-split0-rs1-rm1-off.hash")
n=0
for f in "$out"/*.hash; do
  n=$((n + 1))
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "pictures identical across $n runs: $ref"
[[ "$(cat "$out/rm0-learn.records")" == "$(cat "$out/rm0-off.records")" && "$(cat "$out/rm0-learn1.bytes")" == 0 ]] || { echo "FAIL: learning with the master switch off changed something"; exit 1; }
# the draw records and the bytes copied
for soft in 128 0; do
  for split in 0 1 2; do
    for rs in 0 1 2; do
      for rm in 1 2; do
        t="soft$soft-split$split-rs$rs-rm$rm"
        [[ -f "$out/$t-off.records" ]] || continue
        off=$(cat "$out/$t-off.records")
        for n in 256 512 1024; do
          all=$(cat "$out/$t-dlmax$n.records") learn=$(cat "$out/$t-learn$n.records")
          [[ $all -le $off ]] || { echo "FAIL: $t dlmax$n: $all records, more than $off"; exit 1; }
          # (a learned list is copied after a frame or two; the uses it has not learned anything about, or whose copy joined nothing, are a few records more)
          [[ $learn -lt $off && $learn -le $((all + 3)) ]] || { echo "FAIL: $t learn$n: $learn records, always copied has $all, off $off"; exit 1; }
          [[ $(cat "$out/$t-learn$n.bytes") -lt $(cat "$out/$t-dlmax$n.bytes") ]] || { echo "FAIL: $t learn$n copied $(cat "$out/$t-learn$n.bytes") bytes, not fewer than dlmax$n's $(cat "$out/$t-dlmax$n.bytes")"; exit 1; }
        done
        [[ $(cat "$out/$t-learn1.records") == $(cat "$out/$t-learn512.records") ]] || { echo "FAIL: $t DLLEARN=1 is not the 512 cap"; exit 1; }
      done
    done
  done
  t="soft$soft-split0-rs1-rm1"
  echo "GL records of the last frame (GXV_SOFTXFORM=$soft): off $(cat "$out/$t-off.records"); always copied (256 / 512 / 1024): $(cat "$out/$t-dlmax256.records") / $(cat "$out/$t-dlmax512.records") / $(cat "$out/$t-dlmax1024.records"); learned: $(cat "$out/$t-learn256.records") / $(cat "$out/$t-learn512.records") / $(cat "$out/$t-learn1024.records")"
  echo "bytes copied in 40 frames: always copied $(cat "$out/$t-dlmax512.bytes"), learned $(cat "$out/$t-learn512.bytes") (cap 512)"
done
echo "PASS"

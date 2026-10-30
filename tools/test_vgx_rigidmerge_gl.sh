#!/usr/bin/env bash
# GXV_RIGIDMERGE (rigid draws that differ only in their model-view matrix become one draw with the matrix palette): the production GX front
# end and the production GLES back end on Mesa (tests/vgx_rigidmerge_gl.cpp). Many rigid objects with mixed states are drawn; the picture
# must be bit-identical with the switch off (0, unset, invalid) and on (1: up to 10 matrices a draw, 2: up to 4), under the other
# front-end switches (GXV_SPLIT 0/1/2, GXV_RSCACHE 0/1/2, GXV_SOFTXFORM 0/128, GXV_UNIPACK 0), and the number of GL draws (records) must
# be the model's; the draws are counted and the reduction is printed.
# Also checks that the vertex shader texts are byte-identical to the generator before the palette work (digest made with it): this switch does not
# change the generator.
#   tools/test_vgx_rigidmerge_gl.sh [output-dir]       (no game data, no Vita)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_RIGIDMERGE_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_rigidmerge_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_rigidmerge_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_UNIPACK -u GXV_SOFTXFORM LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_rigidmerge_gl" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -E "FAIL|^draws|^rigid runs" "$out/$tag.log" | sed "s/^/  [$tag] /" | grep -E "FAIL|rigid runs|draws" || true
  grep -q FAIL "$out/$tag.log" && { echo "FAILED: $tag"; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
}
# The vertex shader texts are those of the generator before the palette work (this switch adds no shader text).
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_palrows_text.cpp" "$src/vgx_shadergen.cpp" -o "$out/vgx_palrows_text"
"$out/vgx_palrows_text" b70dbade3be9baea
for soft in 128 0; do
  for split in 0 1 2; do
    for rs in 0 1 2; do
      for mode in unset 0 bogus 1 2; do
        tag="soft$soft-split$split-rs$rs-$mode"
        if [[ $mode == unset ]]; then run "$tag" GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_RSCACHE=$rs
        else run "$tag" GXV_RIGIDMERGE=$mode GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_RSCACHE=$rs; fi
      done
    done
  done
done
run nounipack-0 GXV_RIGIDMERGE=0 GXV_UNIPACK=0
run nounipack-1 GXV_RIGIDMERGE=1 GXV_UNIPACK=0 # (no packing: the switch does nothing)
ref=$(cat "$out/soft128-split0-rs1-unset.hash")
n=0
for f in "$out"/*.hash; do
  n=$((n + 1))
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "pictures identical across $n runs: $ref"
# GL draws (records): the same for every front-end variant of a value of the switch, fewer with the switch on
for soft in 128 0; do
  off=$(cat "$out/soft$soft-split0-rs1-0.records")
  for mode in unset bogus 0; do
    for split in 0 1 2; do for rs in 0 1 2; do
      [[ "$(cat "$out/soft$soft-split$split-rs$rs-$mode.records")" == "$off" ]] || { echo "FAIL: soft$soft split$split rs$rs $mode: draws differ from the switch-off count $off"; exit 1; }
    done; done
  done
  for mode in 1 2; do
    n=$(cat "$out/soft$soft-split0-rs1-$mode.records")
    for split in 0 1 2; do for rs in 0 1 2; do
      [[ "$(cat "$out/soft$soft-split$split-rs$rs-$mode.records")" == "$n" ]] || { echo "FAIL: soft$soft split$split rs$rs mode $mode: draws differ from $n"; exit 1; }
    done; done
    [[ $n -lt $off ]] || { echo "FAIL: soft$soft mode $mode: $n draws, not fewer than $off"; exit 1; }
  done
  echo "GL draws in the test scene (GXV_SOFTXFORM=$soft): off $off, GXV_RIGIDMERGE=1 $(cat "$out/soft$soft-split0-rs1-1.records"), GXV_RIGIDMERGE=2 $(cat "$out/soft$soft-split0-rs1-2.records")"
done
echo "PASS"

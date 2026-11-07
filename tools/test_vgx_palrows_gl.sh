#!/usr/bin/env bash
# GXV_PALROWS (small matrix palette for draws whose vertices name no matrix row above 9): the production GX front end and the
# production GLES back end on Mesa (tests/vgx_palrows_gl.cpp). A skinned, lit mesh is drawn in groups of different highest matrix
# row; the renderer's picture must be bit-identical with the switch off (0, unset, 2 = census only, invalid) and on (1), under
# the other front-end switches (GXV_SPLIT 0/1, GXV_RSCACHE 0/1/2, GXV_UNIPACK 0), and the palette sizes must be the expected ones.
# Also checks that the vertex shader text is byte-identical to the previous generator for the short and the full palette.
#   tools/test_vgx_palrows_gl.sh [output-dir]       (no game data, no Vita)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_PALROWS_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_palrows_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_palrows_gl"
fi
run() { # <expected-mode> <tag> env assignments...
  local expect=$1 tag=$2; shift 2
  env -u GXV_PALROWS -u GXV_SPLIT -u GXV_RSCACHE -u GXV_UNIPACK LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_palrows_gl" "$expect" 2>&1 | grep -v '^libunwind' | tee "$out/$tag.log" | grep -E "FAIL|^picture|palette draws" || true
  grep -q FAIL "$out/$tag.log" && { echo "FAILED: $tag"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
}
# The vertex shader texts without the small palette are byte-identical to the generator before the switch (digest made with it).
nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_palrows_text.cpp" "$src/vgx_shadergen.cpp" -o "$out/vgx_palrows_text"
"$out/vgx_palrows_text" b70dbade3be9baea
ref=""
for split in 0 1 2; do
  for rs in 0 1 2; do
    for mode in unset 0 2 bogus; do
      tag="off-$mode-split$split-rs$rs"
      if [[ $mode == unset ]]; then run 0 "$tag" GXV_SPLIT=$split GXV_RSCACHE=$rs
      else run 0 "$tag" GXV_PALROWS=$mode GXV_SPLIT=$split GXV_RSCACHE=$rs; fi
    done
    run 1 "on-split$split-rs$rs" GXV_PALROWS=1 GXV_SPLIT=$split GXV_RSCACHE=$rs
  done
done
run 2 off-nounipack GXV_PALROWS=0 GXV_UNIPACK=0
run 2 on-nounipack GXV_PALROWS=1 GXV_UNIPACK=0 # (no packing: the palette stays the full one; nothing changes)
ref=$(cat "$out/off-unset-split0-rs1.hash")
for f in "$out"/*.hash; do
  [[ $f == *nounipack* ]] && continue
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
echo "pictures identical across $(ls "$out"/*.hash | grep -vc nounipack) runs: $ref"
[[ "$(cat "$out/off-nounipack.hash")" == "$(cat "$out/on-nounipack.hash")" ]] || { echo "FAIL: unpacked runs differ"; exit 1; }
echo "PASS"

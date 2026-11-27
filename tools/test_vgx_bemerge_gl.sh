#!/usr/bin/env bash
# GXV_BE_MERGE (adjacent draw commands that are one draw call) and the GXV_LIGHTBAKE census: the production GX front end and the
# production GLES back end on Mesa (tests/vgx_bemerge_gl.cpp). Display lists drawn under many matrices and four materials; the test cuts
# every triangle record into two or three ADJACENT records (BEMERGE_SPLIT=1; =2 gives the second parts copies of the uniform blocks at
# other offsets) at triangle boundaries, so that the picture of every frame must be the one the unsplit records give, bit for bit, with
# GXV_BE_MERGE=0 (several draw calls), 1 (merged) and 2 (census only); with BEMERGE_VARY=1..5 the second part of every fifth record
# differs in one thing the merge must compare (cull mode, the texture, the model-view block's VALUES, a gap in the index range, the fragment
# program): the pictures then must equal between GXV_BE_MERGE=0 and 1. The merge must have taken records out (the "be-merge:" line).
# GXV_LIGHTBAKE=2 must report most lighting inputs of the static-buffer draws unchanged for BEMERGE_STILL=1 (a draw or two a picture changes its buffer or arena as lists are promoted) and none for a moving camera.
#   tools/test_vgx_bemerge_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
# Extra compiler flags (e.g. -fsanitize=address,undefined) in BEMERGE_CXXFLAGS; BEMERGE_FRAMES (default 320: the census line comes every 300).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
frames=${BEMERGE_FRAMES:-320}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${BEMERGE_SKIP_BUILD:-0}" != 1 ]]; then
  # shellcheck disable=SC2086
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g ${BEMERGE_CXXFLAGS:-} -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_bemerge_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_bemerge_gl"
fi
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_BE_MERGE -u GXV_LIGHTBAKE -u BEMERGE_SPLIT -u BEMERGE_VARY -u BEMERGE_STILL -u GXV_SPLIT -u GXV_RSCACHE -u GXV_ABSIDX \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_bemerge_gl" "$frames" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH|runtime error|ERROR: AddressSanitizer" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH|runtime error|ERROR" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  sed -n 's/^picture .* hash //p' "$out/$tag.log" > "$out/$tag.hash"
}
removed() { sed -n 's/.*draw calls removed \([0-9.]*\) .*/\1/p' "$out/$1.log" | head -1; }
same() { cmp -s "$out/$1.hash" "$out/$2.hash" || { echo "FAILED: $1 and $2 differ ($(cat "$out/$1.hash") / $(cat "$out/$2.hash"))"; exit 1; }; }
n=0
for fe in "" "GXV_SPLIT=0" "GXV_SPLIT=1" "GXV_ABSIDX=0" "GXV_RSCACHE=2"; do
  t=${fe:-default}; t=${t//=/}
  run "ref-$t" $fe BEMERGE_SPLIT=0 GXV_BE_MERGE=0
  for split in 1 2; do
    run "s$split-m0-$t" $fe BEMERGE_SPLIT=$split GXV_BE_MERGE=0; same "ref-$t" "s$split-m0-$t"
    run "s$split-m1-$t" $fe BEMERGE_SPLIT=$split GXV_BE_MERGE=1; same "ref-$t" "s$split-m1-$t"
    run "s$split-m2-$t" $fe BEMERGE_SPLIT=$split GXV_BE_MERGE=2; same "ref-$t" "s$split-m2-$t"
    r=$(removed "s$split-m1-$t"); [[ -n "$r" ]] && awk "BEGIN{exit !($r > 5)}" || { echo "FAILED: s$split-m1-$t merged too few draws a picture ($r)"; exit 1; }
    n=$((n + 3))
  done
  run "ref-m1-$t" $fe BEMERGE_SPLIT=0 GXV_BE_MERGE=1; same "ref-$t" "ref-m1-$t"
  for v in 1 2 3 4 5; do
    run "v$v-m0-$t" $fe BEMERGE_SPLIT=1 BEMERGE_VARY=$v GXV_BE_MERGE=0
    run "v$v-m1-$t" $fe BEMERGE_SPLIT=1 BEMERGE_VARY=$v GXV_BE_MERGE=1; same "v$v-m0-$t" "v$v-m1-$t"
    n=$((n + 2))
  done
done
# the census of the lighting inputs: a still scene reports (nearly) all unchanged, a moving camera (nearly) none
run lb-still BEMERGE_STILL=1 GXV_LIGHTBAKE=2
run lb-move GXV_LIGHTBAKE=2
pat='s/.*lightbake:.*static buffers [0-9.]*k indices = \([0-9.]*\)% of theirs.*/\1/p'
st=$(sed -n "$pat" "$out/lb-still.log" | head -1)
mv=$(sed -n "$pat" "$out/lb-move.log" | head -1)
echo "lightbake census: static-buffer lit indices unchanged: still scene ${st:-?}%, moving camera ${mv:-?}%"
awk "BEGIN{exit !(${st:-0} > 60 && ${mv:-100} < 5)}" || { echo "FAILED: lightbake census"; exit 1; }
echo "PASS: $n runs, all pictures identical (GXV_BE_MERGE 0 / 1 / 2, split records, varied second parts)"

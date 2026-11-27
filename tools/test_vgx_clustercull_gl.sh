#!/usr/bin/env bash
# GXV_CLUSTERCULL (vertex25: the clusters of a big static triangle run outside the clip volume are not drawn): the production GX front end and
# the production GLES back end on Mesa (tests/vgx_staticcull_gl.cpp, whose scene now holds four 14 000-index terrain sheets in strip order, partly
# in view of a turning, walking camera, in the three material classes). The picture of every frame must be bit-identical with the switch off (0),
# on (1, 3), dry (2), at the default thresholds and at aggressive ones (clusters of 16 triangles, gaps of 300 indices: interior gaps = extra draws),
# under the other front-end switches. With it on indices must be left out and with the aggressive thresholds draws added; the verify counters
# ("range bad" at the draw, "box bad" at the build) must be 0 in mode 3; dry must record exactly as off.
#   tools/test_vgx_clustercull_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
#   SAN=1: build with -fsanitize=address,undefined (ASAN_OPTIONS=detect_leaks=0)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
san=()
[[ "${SAN:-0}" == 1 ]] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
if [[ "${CLUSTER_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g "${san[@]}" -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
    "$root/tests/vgx_staticcull_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_clustercull_gl"
fi
fails=0
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_STATICCULL -u GXV_STATICCULL_PAL -u GXV_DYNCULL -u GXV_DLWELD -u GXV_DLVCACHE -u GXV_DRAWREPLAY -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_SOFTXFORM -u GXV_ABSIDX -u GXV_CHARAA \
    -u GXV_CLUSTERCULL -u GXV_CLUSTERCULL_GAP -u GXV_CLUSTERCULL_HEAD -u GXV_CLUSTERCULL_TRIS -u GXV_CLUSTERCULL_MINRUN -u GXV_VS_LTSKIP -u GXV_VS_TRIM -u STATICCULL_LIGHTS \
    ASAN_OPTIONS=detect_leaks=0 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_clustercull_gl" 30 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH|runtime error|AddressSanitizer" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH|runtime error|AddressSanitizer" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records ' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
  grep -E '^clleft ' "$out/$tag.log" | sed 's/clleft //' > "$out/$tag.left"
  grep -E '^clbad ' "$out/$tag.log" | sed 's/clbad //' > "$out/$tag.bad"
}
same() { # <ref> <tag>: same pictures; bad counters 0
  cmp -s "$out/$1.hash" "$out/$2.hash" || { echo "MISMATCH: $2 differs from $1 ($(cat "$out/$1.hash") vs $(cat "$out/$2.hash"))"; fails=$((fails + 1)); }
  [[ $(cat "$out/$2.bad") == 0 ]] || { echo "BAD COUNTER: $2 ($(cat "$out/$2.bad"))"; fails=$((fails + 1)); }
}
n=0
for soft in 128 0; do
  for split in 0 1 2; do
    for abs in 1 0; do
      for weld in 1 0; do
        [[ $soft == 0 && ($split != 1 || $abs != 1) ]] && continue
        base=(GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_ABSIDX=$abs GXV_DLWELD=$weld GXV_RSCACHE=1 GXV_RIGIDMERGE=1)
        t="s$soft-p$split-a$abs-w$weld"
        run "$t-off" "${base[@]}" GXV_CLUSTERCULL=0
        run "$t-on" "${base[@]}" GXV_CLUSTERCULL=1
        run "$t-ver" "${base[@]}" GXV_CLUSTERCULL=3
        run "$t-agg" "${base[@]}" GXV_CLUSTERCULL=3 GXV_CLUSTERCULL_TRIS=16 GXV_CLUSTERCULL_GAP=300 GXV_CLUSTERCULL_HEAD=96 GXV_CLUSTERCULL_MINRUN=300
        run "$t-dry" "${base[@]}" GXV_CLUSTERCULL=2 GXV_CLUSTERCULL_TRIS=16 GXV_CLUSTERCULL_GAP=300 GXV_CLUSTERCULL_HEAD=96 GXV_CLUSTERCULL_MINRUN=300
        for v in on ver agg dry; do same "$t-off" "$t-$v"; done
        [[ $(cat "$out/$t-dry.records") == $(cat "$out/$t-off.records") ]] || { echo "dry changed the records: $t"; fails=$((fails + 1)); }
        [[ $(cat "$out/$t-agg.left") -gt 0 ]] || { echo "nothing left out at aggressive thresholds: $t"; fails=$((fails + 1)); }
        [[ $(cat "$out/$t-off.left") == 0 ]] || { echo "left out with the switch off: $t"; fails=$((fails + 1)); }
        n=$((n + 5))
      done
    done
  done
done
# other switches in play: vertex-cache order (the run draws its reordered block: clusters stand aside), culling off, draw replay, learned copies are off
for extra in "GXV_DLVCACHE=1 GXV_DLWELD=1" "GXV_STATICCULL=0" "GXV_DRAWREPLAY=2" "GXV_DRAWREPLAY=0" "GXV_CHARAA=1" "GXV_STATICCULL_PAL=1 GXV_DYNCULL=1" "GXV_RIGIDMERGE=0"; do
  t="x-$(echo "$extra" | tr ' =' '__')"
  # shellcheck disable=SC2086
  run "$t-off" $extra GXV_CLUSTERCULL=0
  # shellcheck disable=SC2086
  run "$t-agg" $extra GXV_CLUSTERCULL=3 GXV_CLUSTERCULL_TRIS=16 GXV_CLUSTERCULL_GAP=300 GXV_CLUSTERCULL_HEAD=96 GXV_CLUSTERCULL_MINRUN=300
  same "$t-off" "$t-agg"
  n=$((n + 2))
done
# GXV_VS_LTSKIP (a light of colour exactly 0 is skipped by a uniform branch in the vertex shader): STATICCULL_LIGHTS=1 a black light in the colour channel,
# 2 rgb black with alpha live in colour and alpha channels (the merged loop), 3 a live light; pictures identical with the switch on, with and without GXV_VS_TRIM
for lights in 1 2 3; do
  for trim in 0 1; do
    run "lt$lights-t$trim-off" STATICCULL_LIGHTS=$lights GXV_VS_TRIM=$trim GXV_VS_LTSKIP=0
    run "lt$lights-t$trim-on" STATICCULL_LIGHTS=$lights GXV_VS_TRIM=$trim GXV_VS_LTSKIP=1
    same "lt$lights-t$trim-off" "lt$lights-t$trim-on"
    n=$((n + 2))
  done
done
cmp -s "$out/lt1-t0-off.hash" "$out/lt3-t0-off.hash" && { echo "the second light changes nothing: the LTSKIP test does not exercise it"; fails=$((fails + 1)); }
echo "left out (indices over 30 frames), aggressive, s128-p1-a1-w1: $(cat "$out/s128-p1-a1-w1-agg.left"); default thresholds: $(cat "$out/s128-p1-a1-w1-on.left"); records off/on/agg: $(cat "$out/s128-p1-a1-w1-off.records") / $(cat "$out/s128-p1-a1-w1-on.records") / $(cat "$out/s128-p1-a1-w1-agg.records")"
grep -h "^cluster runs" "$out/s128-p1-a1-w1-agg.log" "$out/s128-p1-a1-w1-on.log"
[[ $fails == 0 ]] && echo "PASS: $n runs, every picture identical, verify counters 0" || { echo "FAIL: $fails problem(s)"; exit 1; }

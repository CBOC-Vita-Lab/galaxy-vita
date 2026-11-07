#!/usr/bin/env bash
# GXV_STATICCULL (a static run of a cached display list whose bounds are outside the clip volume is not drawn): the production GX front
# end and the production GLES back end on Mesa (tests/vgx_staticcull_gl.cpp). Rigid objects all around a turning, walking camera
# (beside, above, below, behind it, straddling the planes, through the near plane, perspective and orthographic, two matrix slots,
# the material changing at culled draws); the pictures of every frame must be bit-identical with the switch off (0) and on (1, unset:
# the default), under the other front-end switches (GXV_SPLIT 0/1/2, GXV_RSCACHE 0/1/2, GXV_RIGIDMERGE 0/1/2, GXV_SOFTXFORM 128/0,
# GXV_ABSIDX 0/1, GXV_CHARAA 0/1, GXV_DRAWREPLAY 0/2); with it on runs must be culled and the draw records must be fewer, and with it off none culled
# and the records the same in every variant.
# REF_ROOT=<tree without the switch> (e.g. /source before this change): the same scene is also drawn by that tree's front end, and its
# pictures must be the same as well.
#   tools/test_vgx_staticcull_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_STATICCULL_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_staticcull_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_staticcull_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_STATICCULL -u GXV_DRAWREPLAY -u GXV_RIGIDMERGE -u GXV_SPLIT -u GXV_RSCACHE -u GXV_SOFTXFORM -u GXV_ABSIDX -u GXV_CHARAA \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_staticcull_gl" 30 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL|MISMATCH" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^records ' "$out/$tag.log" | sed 's/records //' > "$out/$tag.records"
  grep -E '^culled ' "$out/$tag.log" | sed 's/culled //' > "$out/$tag.culled"
}
n=0
for soft in 128 0; do
  for split in 0 1 2; do
    for rs in 0 1 2; do
      for rm in 0 1 2; do
        [[ $soft == 0 && $rs != 1 ]] && continue
        [[ $rm != 1 && $rs != 1 ]] && continue
        base=(GXV_RIGIDMERGE=$rm GXV_SOFTXFORM=$soft GXV_SPLIT=$split GXV_RSCACHE=$rs)
        t="soft$soft-split$split-rs$rs-rm$rm"
        run "$t-off" "${base[@]}" GXV_STATICCULL=0
        run "$t-on" "${base[@]}" GXV_STATICCULL=1
        n=$((n + 2))
      done
    done
  done
done
run default
for rp in 0 2; do # (GXV_DRAWREPLAY: the geometry calls that repeat the previous picture's go straight to their cache entry)
  run "replay$rp-off" GXV_DRAWREPLAY=$rp GXV_STATICCULL=0
  run "replay$rp-on" GXV_DRAWREPLAY=$rp GXV_STATICCULL=1
done
for abs in 0 1; do
  for cha in 0 1; do
    run "abs$abs-cha$cha-off" GXV_ABSIDX=$abs GXV_CHARAA=$cha GXV_STATICCULL=0
    run "abs$abs-cha$cha-on" GXV_ABSIDX=$abs GXV_CHARAA=$cha GXV_STATICCULL=1
  done
done
ref=$(cat "$out/soft128-split0-rs1-rm1-off.hash")
n=0
for f in "$out"/*.hash; do
  n=$((n + 1))
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $ref"; exit 1; }
done
if [[ -n "${REF_ROOT:-}" ]]; then
  # (the reference tree has no counters: a stub that reports none)
  printf '#include <cstdint>\nnamespace vgx { void StaticCullCounters(uint64_t o[3]) { o[0] = o[1] = o[2] = 0; } }\n' > "$out/ref_stub.cpp"
  rargs=()
  for unit in "${sources[@]}"; do rargs+=("$REF_ROOT/platform/vgx/src/$unit.cpp"); done
  grep -q StaticCullCounters "$REF_ROOT/platform/vgx/src/vgx_state.h" || rargs+=("$out/ref_stub.cpp")
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$REF_ROOT/platform/vgx/src" -I"$REF_ROOT/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_staticcull_gl.cpp" "${rargs[@]}" "$REF_ROOT/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_staticcull_gl_ref"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$out/vgx_staticcull_gl_ref" 30 2>&1 | grep -v '^libunwind' > "$out/ref.log" || true
  rh=$(grep -E '^picture' "$out/ref.log" | sed 's/.*hash //')
  [[ "$rh" == "$ref" ]] || { echo "FAIL: the reference tree $REF_ROOT draws $rh, this one $ref"; exit 1; }
  [[ "$(grep -E '^records ' "$out/ref.log" | sed 's/records //')" == "$(cat "$out/soft128-split0-rs1-rm1-off.records")" ]] || { echo "FAIL: the switch off does not make the reference tree's records"; exit 1; }
  echo "the reference tree ($REF_ROOT) draws the same pictures, and its records are the switch-off ones"
fi
echo "pictures identical across $n runs: $ref"
# culled with the switch on (and by default), never with it off; fewer records
for f in "$out"/*-off.culled; do [[ "$(cat "$f")" == 0 ]] || { echo "FAIL: $(basename "$f" .culled) culled $(cat "$f") runs with the switch off"; exit 1; }; done
for f in "$out"/*-on.culled "$out/default.culled"; do [[ "$(cat "$f")" -gt 0 ]] || { echo "FAIL: $(basename "$f" .culled) culled nothing"; exit 1; }; done
[[ "$(cat "$out/default.records")" == "$(cat "$out/soft128-split0-rs1-rm1-on.records")" ]] || { echo "FAIL: the default is not GXV_STATICCULL=1"; exit 1; }
for f in "$out"/*-on.records; do
  t=$(basename "$f" -on.records)
  [[ $(cat "$f") -lt $(cat "$out/$t-off.records") ]] || { echo "FAIL: $t: $(cat "$f") records with the switch on, not fewer than $(cat "$out/$t-off.records")"; exit 1; }
done
for soft in 128 0; do
  for rm in 0 1 2; do
    [[ -f "$out/soft$soft-split0-rs1-rm$rm-on.records" ]] || continue
    a=$(cat "$out/soft$soft-split0-rs1-rm$rm-on.records") b=$(cat "$out/soft$soft-split0-rs1-rm$rm-off.records")
    for split in 1 2; do
      [[ "$(cat "$out/soft$soft-split$split-rs1-rm$rm-on.records")" == "$a" && "$(cat "$out/soft$soft-split$split-rs1-rm$rm-off.records")" == "$b" ]] || \
        { echo "FAIL: soft$soft rm$rm: the records differ between GXV_SPLIT values"; exit 1; }
    done
    echo "draw records over 30 frames (GXV_SOFTXFORM=$soft, GXV_RIGIDMERGE=$rm): off $b, on $a; static runs culled $(cat "$out/soft$soft-split0-rs1-rm$rm-on.culled")"
  done
done
echo "PASS"

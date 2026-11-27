#!/usr/bin/env bash
# GXV_OCCLCULL (draws hidden behind a closed occluder's inscribed ball are not drawn) and GXV_GEOPASS (the census): the production GX
# front end and the production GLES back end on Mesa (tests/vgx_occlcull_gl.cpp). A bumpy closed planet drawn first, objects all
# around and inside it; an inverted planet, an unevenly scaled one, one with an alpha test, an open bowl (none of which may occlude);
# mid-picture depth clears, a far depth-ALWAYS backdrop, another viewport and scissor; a camera that orbits, comes close and looks past.
# The pictures of every frame must be bit-identical with the switch off (0), on (1) and counting (2), under the other front-end
# switches (GXV_SPLIT 0/1/2, GXV_RIGIDMERGE 0/1, GXV_SOFTXFORM 128/0, GXV_STATICCULL 0/1 with GXV_STATICCULL_PAL / GXV_DYNCULL,
# GXV_ABSIDX 0/1, GXV_DLWELD 0/1, GXV_STATIC_INST 0/1, GXV_GEOPASS 0/1); with it on, runs must be left out and the records fewer; with it
# off or counting, the records are the same.
# GXV_ATLAS=0: with copy atlases the picture copy lands in an atlas cell and the copy's own texture (what the test reads back) keeps
# the first picture: the comparison would only see that one (the test checks that the pictures differ from frame to frame).
# REF_ROOT=<tree without the switch> (e.g. the base tree): the same scene drawn by that tree's front end must give the same pictures.
#   tools/test_vgx_occlcull_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_OCCL_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_occlcull_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_occlcull_gl"
fi
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_OCCLCULL -u GXV_GEOPASS -u GXV_STATICCULL -u GXV_STATICCULL_PAL -u GXV_DYNCULL -u GXV_DLWELD -u GXV_RIGIDMERGE -u GXV_SPLIT \
    -u GXV_SOFTXFORM -u GXV_ABSIDX -u GXV_STATIC_INST \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 GXV_ATLAS=0 \
    "$@" "$out/vgx_occlcull_gl" 32 > "$out/$tag.log" 2>&1 || true
  grep -q -E "FAIL|MISMATCH" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|MISMATCH" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  for k in records registered occlstatic occldyn closed breaks; do grep -E "^$k " "$out/$tag.log" | sed "s/^$k //" > "$out/$tag.$k"; done
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
}
for soft in 128 0; do
  for split in 0 1 2; do
    for rm in 0 1; do
      base=(GXV_RIGIDMERGE=$rm GXV_SOFTXFORM=$soft GXV_SPLIT=$split)
      t="soft$soft-split$split-rm$rm"
      run "$t-off" "${base[@]}" GXV_OCCLCULL=0
      run "$t-on" "${base[@]}" GXV_OCCLCULL=1
      run "$t-count" "${base[@]}" GXV_OCCLCULL=2
    done
  done
done
for sc in 0 1; do
  run "sc$sc-off" GXV_STATICCULL=$sc GXV_STATICCULL_PAL=$sc GXV_DYNCULL=$sc GXV_OCCLCULL=0
  run "sc$sc-on" GXV_STATICCULL=$sc GXV_STATICCULL_PAL=$sc GXV_DYNCULL=$sc GXV_OCCLCULL=1
done
for abs in 0 1; do
  for weld in 0 1; do
    run "abs$abs-weld$weld-off" GXV_ABSIDX=$abs GXV_DLWELD=$weld GXV_OCCLCULL=0
    run "abs$abs-weld$weld-on" GXV_ABSIDX=$abs GXV_DLWELD=$weld GXV_OCCLCULL=1 GXV_GEOPASS=1
  done
done
run "inst-off" GXV_STATIC_INST=1 GXV_OCCLCULL=0
run "inst-on" GXV_STATIC_INST=1 GXV_OCCLCULL=1
run "geo-only" GXV_GEOPASS=1
run "default"
# (GXV_SOFTXFORM=0 moves the small objects' transform from the CPU to the vertex shader: other rounding, another picture, with the
# switch off as well; those runs are compared among themselves)
ref=$(cat "$out/soft128-split0-rm1-off.hash")
ref0=$(cat "$out/soft0-split0-rm1-off.hash")
n=0
for f in "$out"/*.hash; do
  n=$((n + 1))
  want=$ref
  case $(basename "$f") in soft0-*) want=$ref0 ;; esac
  [[ "$(cat "$f")" == "$want" ]] || { echo "FAIL: $(basename "$f" .hash) picture $(cat "$f") differs from $want"; exit 1; }
done
echo "pictures identical across $n runs: $ref (GXV_SOFTXFORM=0: $ref0)"
for f in "$out"/*-on.occlstatic; do
  t=$(basename "$f" .occlstatic)
  [[ "$(cat "$f")" -gt 0 ]] || { echo "FAIL: $t: no static run left out"; exit 1; }
  b=${t%-on}
  [[ "$(cat "$out/$t.records")" -lt "$(cat "$out/$b-off.records")" ]] || { echo "FAIL: $t: records $(cat "$out/$t.records") not fewer than $(cat "$out/$b-off.records")"; exit 1; }
done
for f in "$out"/*-off.occlstatic "$out/default.occlstatic" "$out/geo-only.occlstatic"; do
  [[ "$(cat "$f")" == 0 ]] || { echo "FAIL: $(basename "$f" .occlstatic): left out with the switch off"; exit 1; }
done
for f in "$out"/*-count.records; do
  t=$(basename "$f" -count.records)
  [[ "$(cat "$f")" == "$(cat "$out/$t-off.records")" ]] || { echo "FAIL: $t: GXV_OCCLCULL=2 changed the records"; exit 1; }
  [[ "$(cat "$out/$t-count.occlstatic")" == "$(cat "$out/$t-on.occlstatic")" ]] || { echo "FAIL: $t: =2 counts $(cat "$out/$t-count.occlstatic"), =1 left out $(cat "$out/$t-on.occlstatic")"; exit 1; }
done
[[ "$(cat "$out/default.records")" == "$(cat "$out/soft128-split0-rm1-off.records")" ]] || echo "note: default records differ from rm1 off (other defaults)"
[[ "$(cat "$out/soft128-split0-rm1-on.occldyn")" -gt 0 ]] || { echo "FAIL: no dynamic draw left out (GXV_SOFTXFORM=128, GXV_SPLIT=0)"; exit 1; }
echo "left out over 32 pictures (GXV_SOFTXFORM=128, GXV_SPLIT=0, GXV_RIGIDMERGE=1): static runs $(cat "$out/soft128-split0-rm1-on.occlstatic"), dynamic draws $(cat "$out/soft128-split0-rm1-on.occldyn"); records $(cat "$out/soft128-split0-rm1-off.records") -> $(cat "$out/soft128-split0-rm1-on.records"); occluders registered $(cat "$out/soft128-split0-rm1-on.registered"), closed runs $(cat "$out/soft128-split0-rm1-on.closed"), epochs ended $(cat "$out/soft128-split0-rm1-on.breaks")"
if [[ -n "${REF_ROOT:-}" ]]; then
  printf '#include <cstdint>\nnamespace vgx { void OcclCounters(uint64_t o[8]) { for (int i = 0; i < 8; ++i) o[i] = 0; } }\n' > "$out/ref_stub.cpp"
  rargs=()
  for unit in "${sources[@]}"; do rargs+=("$REF_ROOT/platform/vgx/src/$unit.cpp"); done
  grep -q OcclCounters "$REF_ROOT/platform/vgx/src/vgx_state.h" || rargs+=("$out/ref_stub.cpp")
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$REF_ROOT/platform/vgx/src" -I"$REF_ROOT/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_occlcull_gl.cpp" "${rargs[@]}" "$REF_ROOT/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_occlcull_gl_ref"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 GXV_ATLAS=0 GXV_RIGIDMERGE=1 \
    GXV_SOFTXFORM=128 GXV_SPLIT=0 "$out/vgx_occlcull_gl_ref" 32 > "$out/ref.log" 2>&1 || true
  rh=$(grep -E '^picture' "$out/ref.log" | sed 's/.*hash //')
  [[ "$rh" == "$ref" ]] || { echo "FAIL: the reference tree $REF_ROOT draws $rh, this one $ref"; exit 1; }
  [[ "$(grep -E '^records ' "$out/ref.log" | sed 's/records //')" == "$(cat "$out/soft128-split0-rm1-off.records")" ]] || { echo "FAIL: the switch off does not make the reference tree's records"; exit 1; }
  echo "the reference tree ($REF_ROOT) draws the same pictures, and its records are the switch-off ones"
fi
echo "PASS"

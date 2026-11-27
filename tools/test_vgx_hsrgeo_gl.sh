#!/usr/bin/env bash
# GXV_HSR_ORDER_TIES=4 (ties25) on Mesa: tests/vgx_hsrgeo_gl.cpp, static quads through the display-list cache and the GLES back end (see the file's header for the
# seven frames). Hashes of the pictures of GXV_HSR_ORDER=1 with TIES 4 (and 6) and the census equal the original order's; ties 1 moves nothing (everything shares a
# transform) and ties 4 moves the separated quads; NEGATIVE CONTROLS: TIES=0 and the proofs' pins removed (GXV_HSR_GEO_TEST=1) change the tie frames (2, 3, 4).
#   tools/test_vgx_hsrgeo_gl.sh [output-dir]      (needs CPLUS_INCLUDE_PATH=<tree>/platform/include; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$root/platform/vgx/src/$unit.cpp"); done
[[ "${HSR_SKIP_BUILD:-0}" == 1 ]] || nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
    "$root/tests/vgx_hsrgeo_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/geogl"
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0"
run() { # tag, env...
  local tag=$1; shift
  env -u GXV_HSR_ORDER -u GXV_HSR_ORDER_TIES -u GXV_HSR_GEO_TEST -u GXV_HSR_ORDER_ETA $E "$@" "$out/geogl" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL" "$out/$tag.log" && { echo "FAILED: $tag"; grep FAIL "$out/$tag.log" | head; exit 1; }
  grep -q '^frame1.0' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^frame' "$out/$tag.log" | sed 's/ scenes=[0-9]*//' > "$out/$tag.hash"
}
run off
run t1 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=1
run t4 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=4
run t6 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=6
run t4eta GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=4 GXV_HSR_ORDER_ETA=12
run alt GXV_HSR_ORDER=3 GXV_HSR_ORDER_TIES=4
run census GXV_HSR_ORDER=2 GXV_HSR_ORDER_TIES=4 GXV_HSR_ORDER_LOGMS=1
run t0 GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=0
run nocontact GXV_HSR_ORDER=1 GXV_HSR_ORDER_TIES=4 GXV_HSR_GEO_TEST=1
info() { grep -E "^info $2" "$out/$1.log"; }
for t in off t1 t4 t6 alt census t0 nocontact; do echo "$t: $(info $t hsr) | $(info $t geo)"; done
static=$(grep -E '^round 7' "$out/off.log" | sed -E 's/.*static draws ([0-9]+).*/\1/')
[[ ${static:-0} -ge 20 ]] || { echo "FAILED: the quads did not become static buffers (static draws in round 7: ${static:-0})"; exit 1; }
for t in t1 t4 t6 t4eta alt census; do cmp -s "$out/off.hash" "$out/$t.hash" || { echo "FAILED: $t changed a picture"; diff "$out/off.hash" "$out/$t.hash" | head; exit 1; }; done
tm1=$(info t1 hsr | sed -E 's/.*tmoved=([0-9]+).*/\1/'); tm4=$(info t4 hsr | sed -E 's/.*tmoved=([0-9]+).*/\1/'); tm0=$(info t0 hsr | sed -E 's/.*tmoved=([0-9]+).*/\1/')
[[ $tm4 -gt $tm1 && $tm4 -gt 0 ]] || { echo "FAILED: ties 4 moved $tm4 alpha-tested draws, ties 1 moved $tm1 (it must free the separated static quads)"; exit 1; }
rd=$(info t4 geo | sed -E 's/.*ready=([0-9]+).*/\1/'); [[ $rd -gt 0 ]] || { echo "FAILED: no static range was analysed"; exit 1; }
for t in t0 nocontact; do
  cmp -s "$out/off.hash" "$out/$t.hash" && { echo "FAILED: NEGATIVE CONTROL $t should change the tie frames"; exit 1; }
  { diff "$out/off.hash" "$out/$t.hash" || true; } | grep '^>' | sed -E 's/^> frame([0-9]+)\..*/\1/' | sort -un | tr '\n' ' ' > "$out/$t.frames"
  echo "NEGATIVE CONTROL $t changes frames: $(cat "$out/$t.frames")"
  for f in 2 3 4; do grep -qw $f "$out/$t.frames" || { echo "FAILED: $t should change frame $f"; exit 1; }; done
done
echo "ties 4 moved $tm4 alpha-tested draws (ties 1: $tm1, ties 0: $tm0)"
grep -E "hsr25:   (ties 4|geometry|pin):" "$out/census.log" | tail -9 | cut -c1-330
echo "PASS: ties 4 (and 6, other margins, alternating, census) pictures equal the original's for all frames and rounds; ties 4 freed what ties 1 pins ($tm4 vs $tm1 draws moved); ties 0 and the pins removed change the tie frames"

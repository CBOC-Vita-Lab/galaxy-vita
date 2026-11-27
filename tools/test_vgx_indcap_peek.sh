#!/usr/bin/env bash
# GXV_INDCAP_PEEK (peek25): the render thread's lookahead rule, on a two-frame queue through the production front end and GLES back end on Mesa
# (tests/vgx_indcap_peek_gl.cpp), plus the pure rule's unit test (tests/vgx_indcap_peek_test.cpp). Every picture must be identical with the switch off (0),
# counting (2) and applying (1); with 1 the pictures whose capture is dead have fewer scenes; a negative control (a capture left out although the next picture
# reads it) must change a picture, so the comparison can see a stale read.
#   tools/test_vgx_indcap_peek.sh [output-dir]     (Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
"${CXX:-g++}" -std=c++20 -O1 -Wall -I"$src" "$root/tests/vgx_indcap_peek_test.cpp" -o "$out/vgx_indcap_peek_test"
"$out/vgx_indcap_peek_test"
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${INDCAP_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_indcap_peek_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_indcap_peek_gl"
fi
for r in "0:GXV_INDCAP_PEEK=0" "2:GXV_INDCAP_PEEK=2" "1:GXV_INDCAP_PEEK=1" "bad:GXV_INDCAP_PEEK=1 TEST_FORCE_BAD=1"; do
  v=${r%%:*}
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 ${r#*:} "$out/vgx_indcap_peek_gl" > "$out/peek-$v.log" 2>&1 || true
  grep -v "^libunwind" "$out/peek-$v.log" | grep -v "PASS: picture read back" | sed "s/^/[$v] /"
done
grep -q FAIL "$out"/peek-0.log "$out"/peek-1.log "$out"/peek-2.log && { echo "FAILED: a check"; exit 1; }
hashes() { grep -E '^pic[0-9]+ ' "$out/peek-$1.log" | sed -E 's/scenes=[0-9]+ //'; }
scenes() { grep -E "^pic$2 " "$out/peek-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
[[ -n "$(hashes 0)" ]] || { echo "FAILED: no pictures"; exit 1; }
for v in 2 1; do [[ "$(hashes 0)" == "$(hashes $v)" ]] || { echo "FAILED: pictures differ with GXV_INDCAP_PEEK=$v"; diff <(hashes 0) <(hashes $v); exit 1; }; done
[[ "$(hashes 0)" != "$(hashes bad)" ]] || { echo "FAILED: the negative control (a needed capture left out) changed no picture: the comparison is blind"; exit 1; }
for f in 1 3 10 11 12; do
  [[ $(scenes 1 $f) -lt $(scenes 0 $f) ]] || { echo "FAILED: picture $f scenes $(scenes 0 $f) -> $(scenes 1 $f) with PEEK=1 (expected fewer)"; exit 1; }
  [[ $(scenes 2 $f) -eq $(scenes 0 $f) ]] || { echo "FAILED: picture $f scenes changed with PEEK=2"; exit 1; }
done
for f in 2 4 5 7 13 15; do [[ $(scenes 1 $f) -eq $(scenes 0 $f) ]] || { echo "FAILED: picture $f (capture kept) scenes changed"; exit 1; }; done
echo "PASS: 15 pictures identical with GXV_INDCAP_PEEK 0 / 2 / 1; the 5 dead captures (pictures 1 3 10 11 12) have fewer scenes with 1; the needed ones (2 4 5 7 13 15) are made; the negative control differs"

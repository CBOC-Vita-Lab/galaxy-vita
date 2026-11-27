#!/usr/bin/env bash
# GXV_PREVCAP bit 1 (prevcap25): the marked "Indirect" capture answered by the previous picture, through the production front end and GLES back
# end on Mesa (tests/vgx_prevcap_gl.cpp). Proofs:
#   1. default (GXV_PREVCAP unset / 0) pictures are bit-identical to a build without the switch's code path: pictures of the run with the
#      switch at 0 equal the old test's expectations (the capture is made, every picture exact) and the "prevcap" counters stay 0;
#   2. GXV_PREVCAP=1: pictures 2.. are bit-identical to the reference run (reader samples a copy of the previous picture's final EFB), and each
#      has one scene fewer than the run with the switch at 0;
#   3. a camera cut (picture 4) and another stage tag (picture 3) fall back to the real capture: those pictures equal the unchanged run.
#   tools/test_vgx_prevcap_gl.sh [output-dir]   (needs CPLUS_INCLUDE_PATH=<tree>/platform/include; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${PREVCAP_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_prevcap_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_prevcap_gl"
fi
run() { # name, env...
  local n=$1; shift
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 PREVCAP_DUMP="$out/d-$n" "$@" \
    "$out/vgx_prevcap_gl" > "$out/$n.log" 2>&1 || true
  grep -v "^libunwind" "$out/$n.log" | sed "s/^/[$n] /"
}
run off GXV_PREVCAP=0
run unset
run on GXV_PREVCAP=1
run ref GXV_PREVCAP=0 TEST_REF=1
run offcut GXV_PREVCAP=0 TEST_CUT=4
run oncut GXV_PREVCAP=1 TEST_CUT=4
run offstg GXV_PREVCAP=0 TEST_STAGE=3
run onstg GXV_PREVCAP=1 TEST_STAGE=3
run on2 GXV_PREVCAP=2
run sc0 GXV_PREVCAP=0 TEST_NOREAD=1
run sc1 GXV_PREVCAP=1 TEST_NOREAD=1
grep -q FAIL "$out"/*.log && { echo "FAILED: a check"; exit 1; }
h() { grep -E "^pic$2 " "$out/$1.log" | sed -E 's/.*hash=//'; }
s() { grep -E "^pic$2 " "$out/$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
cnt() { grep -E "^info counters" "$out/$1.log" | sed -E "s/.* $2=([0-9]+).*/\1/"; }
fail=0
# 1. off == unset == switch 2 (counted only): every picture the same, nothing counted
for i in 1 2 3 4 5 6 7 8; do
  [[ "$(h off $i)" == "$(h unset $i)" && "$(h off $i)" == "$(h on2 $i)" && "$(s off $i)" == "$(s unset $i)" && "$(s off $i)" == "$(s on2 $i)" ]] || { echo "FAILED: default path differs at picture $i"; fail=1; }
done
[[ "$(cnt off pictures)" == 0 && "$(cnt unset pictures)" == 0 && "$(cnt on2 used)" == 0 ]] || { echo "FAILED: the off path counted something"; fail=1; }
# 2. on: picture 1 as the game; 2.. equal the reference
[[ "$(h on 1)" == "$(h off 1)" ]] || { echo "FAILED: picture 1 (no previous picture yet) must be exact"; fail=1; }
for i in 2 3 4 5 6 7 8; do
  [[ "$(h on $i)" == "$(h ref $i)" ]] || { echo "FAILED: picture $i with GXV_PREVCAP=1 is not the previous-picture reference"; fail=1; }
done
[[ "$(h on 2)" != "$(h off 2)" ]] || { echo "FAILED: picture 2 should differ from the exact capture (one picture old)"; fail=1; }
[[ "$(cnt on used)" == 7 ]] || { echo "FAILED: expected 7 captures left out, got $(cnt on used)"; fail=1; }
# the scenes, the game's own way of ending a picture (the display copy alone): one fewer from picture 2
for i in 2 3 4 5 6 7 8; do [[ $(s sc1 $i) -lt $(s sc0 $i) ]] || { echo "FAILED: picture $i scenes $(s sc0 $i) -> $(s sc1 $i) (expected fewer)"; fail=1; }; done
# 3. fallbacks: the cut's pictures 4-6 and the stage change's 3-4 are the unchanged game's; the rest as the plain run
[[ "$(h oncut 1)" == "$(h off 1)" && "$(h oncut 2)" == "$(h on 2)" && "$(h oncut 3)" == "$(h on 3)" ]] || { echo "FAILED: cut run before the cut"; fail=1; }
for i in 4 5 6; do [[ "$(h oncut $i)" == "$(h offcut $i)" ]] || { echo "FAILED: cut run picture $i differs from the unchanged game"; fail=1; }; done
[[ "$(h oncut 7)" != "$(h offcut 7)" ]] || { echo "FAILED: after the cut gate the previous picture is used again (picture 7)"; fail=1; }
[[ "$(cnt oncut cut)" == 3 ]] || { echo "FAILED: expected 3 pictures refused near the cut, got $(cnt oncut cut)"; fail=1; }
[[ "$(h onstg 1)" == "$(h off 1)" && "$(h onstg 2)" == "$(h on 2)" ]] || { echo "FAILED: stage run before the change"; fail=1; }
for i in 3 4; do [[ "$(h onstg $i)" == "$(h offstg $i)" ]] || { echo "FAILED: stage run picture $i differs from the unchanged game"; fail=1; }; done
[[ "$(h onstg 5)" != "$(h offstg 5)" ]] || { echo "FAILED: the previous picture is used again two pictures after the stage change"; fail=1; }
[[ "$(cnt onstg noPrev)" == 3 ]] || { echo "FAILED: expected pictures 1, 3 and 4 refused (no previous picture of this stage), got $(cnt onstg noPrev)"; fail=1; }
echo "centre pixel per picture: off: $(grep '^centre' $out/off.log | cut -c8-)"
echo "centre pixel per picture: on : $(grep '^centre' $out/on.log | cut -c8-)  (frozen at picture 1's floor: the reader samples its own earlier output)"
echo "scenes per picture (display copy only): off: $(for i in 1 2 3 4 5 6 7 8; do printf '%s ' "$(s sc0 $i)"; done) on: $(for i in 1 2 3 4 5 6 7 8; do printf '%s ' "$(s sc1 $i)"; done)"
[[ $fail == 0 ]] || { echo FAILED; exit 1; }
echo "PASS: GXV_PREVCAP=0/unset/2 identical and uncounted; =1 pictures 2..8 bit-identical to the previous-picture reference, fewer scenes each; cut and stage fallbacks exact"

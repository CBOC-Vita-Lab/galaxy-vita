#!/usr/bin/env bash
# dread25: GXV_ALPHATEST_PREPASS through the production GX front end and GLES back end on Mesa (tests/vgx_hsrorder_gl.cpp: synthetic pictures with opaque, alpha-tested,
# blended and no-depth-write draws, coplanar decals, ties; frame 7 has a LESS tested draw). Pictures are hashed and compared with the switch off.
#   modes 4 / 5 (the colour pass keeps the test): every picture equal to the original, in every frame;
#   modes 1 / 2 (no test in the colour pass): equal except where a killed fragment ties in depth with another surface (the decal frames): the frames that differ are listed;
#   3 (census): nothing changes; and a negative control (GXV_ATPP_BREAK=1: the colour pass ignores the depth) that must change pictures.
# Needs CPLUS_INCLUDE_PATH=<tree>/platform/include (as tools/test_vgx_hsrorder_gl.sh).   tools/test_vgx_prepass_gl.sh [output-dir]   (KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$root/platform/vgx/src/$unit.cpp"); done
[[ "${HSR_SKIP_BUILD:-0}" == 1 ]] || nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
    "$root/tests/vgx_hsrorder_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/hsr"
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0"
run() { # tag, env...
  local tag=$1; shift
  env -u GXV_HSR_ORDER -u GXV_HSR_ORDER_TIES -u GXV_ALPHATEST_PREPASS -u GXV_ATPP_BREAK $E "$@" "$out/hsr" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "FAIL" "$out/$tag.log" && { echo "FAILED: $tag"; grep FAIL "$out/$tag.log" | head; exit 1; }
  grep -q '^frame1.0' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^frame' "$out/$tag.log" | sed 's/ scenes=[0-9]*//' > "$out/$tag.hash"
}
info_() { grep '^info atpp pairs' "$out/$1.log" | sed 's/info atpp //'; }
num() { info_ "$1" | sed -E "s/.*$2=([0-9]+).*/\1/"; }
run off
run m0 GXV_ALPHATEST_PREPASS=0
run m1 GXV_ALPHATEST_PREPASS=1
run m2 GXV_ALPHATEST_PREPASS=2
run m3 GXV_ALPHATEST_PREPASS=3
run m4 GXV_ALPHATEST_PREPASS=4
run m5 GXV_ALPHATEST_PREPASS=5
run m5h GXV_ALPHATEST_PREPASS=5 GXV_HSR_ORDER=1
run m4h GXV_ALPHATEST_PREPASS=4 GXV_HSR_ORDER=1
run brk GXV_ALPHATEST_PREPASS=4 GXV_ATPP_BREAK=1
for t in off m1 m2 m3 m4 m5 m5h brk; do echo "$t: $(info_ $t)"; done
echo "front end (m4): $(grep '^info atppfe' "$out/m4.log")"
cmp -s "$out/off.hash" "$out/m0.hash" || { echo "FAILED: GXV_ALPHATEST_PREPASS=0 differs from unset"; exit 1; }
for t in m3 m4 m5 m5h m4h; do cmp -s "$out/off.hash" "$out/$t.hash" || { echo "FAILED: GXV_ALPHATEST_PREPASS mode $t changed a picture"; diff "$out/off.hash" "$out/$t.hash"; exit 1; }; done
[[ $(num m1 pairs) -gt 0 && $(num m4 pairs) -gt 0 ]] || { echo "FAILED: no draw was split in modes 1 / 4"; exit 1; }
[[ $(num m2 hoisted) -gt 0 && $(num m5 hoisted) -gt 0 ]] || { echo "FAILED: no prepass was hoisted in modes 2 / 5"; exit 1; }
[[ $(num m3 pairs) -eq 0 && $(num m3 planned) -gt 0 ]] || { echo "FAILED: the census (3) split a draw or planned nothing"; exit 1; }
[[ $(num off pairs) -eq 0 && $(num off pics) -eq 0 ]] || { echo "FAILED: the default path ran the planner"; exit 1; }
if cmp -s "$out/off.hash" "$out/brk.hash"; then echo "FAILED: NEGATIVE CONTROL: a colour pass that ignores the depth changed nothing"; exit 1; fi
echo "NEGATIVE CONTROL: a colour pass that ignores the depth changes: $({ diff "$out/off.hash" "$out/brk.hash" || true; } | grep '^>' | sed -E 's/^> (frame[0-9]+)\..*/\1/' | sort -u | tr '\n' ' ')"
for t in m1 m2; do
  fr=$({ diff "$out/off.hash" "$out/$t.hash" || true; } | grep '^>' | sed -E 's/^> (frame[0-9]+)\..*/\1/' | sort -u | tr '\n' ' ')
  echo "mode ${t#m} (no test in the colour pass): frames that differ from the original: ${fr:-none}"
  echo "$fr" > "$out/$t.frames"
done
echo "PASS: modes 3 (census), 4, 5 (also with GXV_HSR_ORDER=1) reproduce every picture; $(num m4 pairs) pairs drawn (mode 4), $(num m5 hoisted) prepasses hoisted (mode 5); the negative control differs"

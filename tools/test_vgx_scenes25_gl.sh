#!/usr/bin/env bash
# GXV_SCENES25 (census) and GXV_SCENES25_DRY (unread captures left out) through the production front end and GLES back end on Mesa
# (tests/vgx_scenes25_gl.cpp). Needs CPLUS_INCLUDE_PATH=<tree>/platform/include (as tools/test_vgx_indcap_gl.sh).
#   tools/test_vgx_scenes25_gl.sh [output-dir]      (Khronos headers: KHR_INCLUDE, default /work/khr/include)
#   S25_BASE_TREE=<clean export of the base commit>: the same test source is built against it (-DS25_BASE) and its pictures and scene
#   counts must equal this tree's with the switches off ("default path bit-identical").
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
build() { # tree, binary, extra flags
  local tree=$1 bin=$2; shift 2
  local args=()
  for unit in "${sources[@]}"; do args+=("$tree/platform/vgx/src/$unit.cpp"); done
  CPLUS_INCLUDE_PATH="$tree/platform/include" nice -n 10 "${CXX:-g++}" -std=c++20 -O1 "$@" -I"$tree/platform/vgx/src" -I"$tree/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_scenes25_gl.cpp" "${args[@]}" "$tree/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$bin"
}
[[ "${S25_SKIP_BUILD:-0}" == 1 ]] || build "$root" "$out/s25" 
if [[ -n "${S25_BASE_TREE:-}" && "${S25_SKIP_BUILD:-0}" != 1 ]]; then build "$S25_BASE_TREE" "$out/s25base" -DS25_BASE; fi
W="GXV_INDCAP_WARMUP=0"
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0"
runs=("0:" "c:GXV_SCENES25=1 GXV_SCENES25_EVERY=5" "dry2:GXV_SCENES25_DRY=7 GXV_INDCAP_SKIP=2 $W" "dryw:GXV_SCENES25_DRY=1 GXV_PASSHOIST=0"
      "h0:GXV_PASSHOIST=0" "hc:GXV_PASSHOIST=0 GXV_SCENES25=1 GXV_SCENES25_EVERY=5 GXV_SCENES25_TRACE=30"
      "hd:GXV_PASSHOIST=0 GXV_SCENES25_DRY=1 $W GXV_SCENES25=1 GXV_SCENES25_EVERY=5" "hd0:GXV_PASSHOIST=0 GXV_SCENES25_DRY=1 GXV_INDCAP_PREDICT=0 GXV_INDCAP_STAGE=0 $W"
      "ind:GXV_PASSHOIST=0 GXV_INDCAP_SKIP=1 $W GXV_INDCAP_HALFRATE=1 GXV_SCENES25_DRY=1")
for r in "${runs[@]}"; do
  v=${r%%:*}
  env $E ${r#*:} "$out/s25" > "$out/s25-$v.log" 2>&1 || true
  grep -v "^libunwind" "$out/s25-$v.log" | grep -v "PASS: picture read back" | sed "s/^/[$v] /"
done
if [[ -x "$out/s25base" ]]; then
  env $E "$out/s25base" > "$out/s25-base.log" 2>&1 || true
  grep -v "^libunwind" "$out/s25-base.log" | grep -v "PASS: picture read back" | sed "s/^/[base] /"
fi
grep -q FAIL "$out"/s25-*.log && { echo "FAILED: a check"; exit 1; }
pics() { grep -E '^frame[1-5] ' "$out/s25-$1.log" | sort; }
sc() { grep -E "^frame$2 " "$out/s25-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
[[ -z "$(pics 0)" ]] && { echo "FAILED: no pictures"; exit 1; }
[[ ! -f "$out/s25-base.log" ]] || [[ "$(pics 0)" == "$(pics base)" ]] || { echo "FAILED: the default path differs from the base tree"; pics 0; pics base; exit 1; }
for v in c dry2; do [[ "$(pics 0)" == "$(pics $v)" ]] || { echo "FAILED: pictures or scene counts differ ($v)"; pics 0; pics $v; exit 1; }; done
# (the dry runs, PASSHOIST off so that the corner copy is a split: frames 1-4 equal to the run without the switch, frames 2 and 4 (no reader of the
# corner) with fewer scenes, frames 1 and 3 unchanged; frame 5 reads the corner texture of a left-out capture: one stale read, counted; the test's alpha is flat, so its picture does not show it)
h() { grep -E "^frame$2 " "$out/s25-$1.log" | sed -E 's/.*hash=//'; }
for v in hc hd; do for f in 1 2 3 4 5; do [[ $(sc $v $f) -ge 1 ]] || { echo "FAILED: no frame $f in run $v"; exit 1; }; done; done
for f in 1 2 3 4 5; do [[ "$(h h0 $f)" == "$(h hd $f)" ]] || { echo "FAILED: dry run frame $f picture differs"; exit 1; }; done
for f in 2 4; do [[ $(sc hd $f) -lt $(sc h0 $f) ]] || { echo "FAILED: dry run frame $f has no fewer scenes ($(sc h0 $f) -> $(sc hd $f))"; exit 1; }; done
for f in 1 3; do [[ $(sc hd $f) -eq $(sc h0 $f) ]] || { echo "FAILED: dry run frame $f scene count changed"; exit 1; }; done
for f in 1 2 3 4 5; do [[ $(sc hc $f) -eq $(sc h0 $f) && "$(h hc $f)" == "$(h h0 $f)" ]] || { echo "FAILED: the census changed frame $f"; exit 1; }; done
grep -q "stale=1" "$out/s25-hd.log" || { echo "FAILED: frame 5's stale read not counted"; exit 1; }
[[ "$(pics h0 | sed -n 1,4p)" == "$(pics dryw | sed -n 1,4p)" ]] || { echo "FAILED: the default warm-up should leave nothing out"; exit 1; }
grep -q "scenes25:" "$out/s25-c.log" || { echo "FAILED: no census line"; exit 1; }
echo "PASS: default path equals the base tree; the census changes no picture or scene count; dry skip (corner, PASSHOIST off): frames 1-5 identical, scenes frame 2 $(sc h0 2) -> $(sc hd 2), frame 4 $(sc h0 4) -> $(sc hd 4), frames 1 and 3 unchanged, frame 5 one stale read"

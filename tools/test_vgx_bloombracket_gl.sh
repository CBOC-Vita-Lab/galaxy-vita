#!/usr/bin/env bash
# GXV_BLOOM_BRACKET (bloom37): the production front end and GLES back end on Mesa (tests/vgx_bloombracket_gl.cpp). Runs the switch off, on (=1) and count-only (=2); every picture (colour AND alpha)
# must be identical in the three runs, =1 must make fewer scenes in frames 1 and 6 (the dead brackets), =2 none fewer; the census checks are the test's own.
#   tools/test_vgx_bloombracket_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include; a host whose compiler rejects the console-only weak declarations of
#   vgx_backend_gl.cpp: GL_TEST_SHIM=<file with `extern "C" void vgxvita_workerAllocStats_v1(uint32_t*, uint32_t*, uint32_t*) __attribute__((weak)); extern "C" uint32_t port_scene_serial(void) __attribute__((weak));`> CXX=clang++)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
obj=${GL_OBJ:-$out/obj}
mkdir -p "$out" "$obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
shim=()
[[ -n "${GL_TEST_SHIM:-}" ]] && shim=(-include "$GL_TEST_SHIM")
flags=(-std=c++20 -O1 -g0 "${shim[@]}" -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr")
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
if [[ "${GL_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h" "$src/vgx_bloombracket.h" "$src/vgx_tailfold.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_bloombracket_gl.cpp" -o "$obj/test.o"
  [[ -f "$obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$obj/test.o" "$obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_bloombracket_gl"
fi
# (h: GXV_BLOOM_BRACKET=1 under GXV_HRW=1: the markers are ignored, nothing is left out, the pictures are the same)
for r in 0 1 2 h; do
  hrw=0; sw=$r
  [[ $r == h ]] && { hrw=1; sw=1; }
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_HRW=$hrw GXV_BLOOM_BRACKET=$sw GXV_BLOOM_BRACKET_LOG=7 "$out/vgx_bloombracket_gl" > "$out/bracket-$r.log" 2>&1 || true
  grep -v "^libunwind" "$out/bracket-$r.log" | grep -v "PASS: picture read back" | sed "s/^/[$r] /"
done
grep -q FAIL "$out"/bracket-*.log && { echo "FAILED: a check"; exit 1; }
pics() { grep -E '^frame[0-9]' "$out/bracket-$1.log" | sed -E "s/scenes=[0-9]+ //"; }
scenes() { grep -E "^frame$2 " "$out/bracket-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
p0=$(pics 0)
[[ -n "$p0" ]] || { echo "FAILED: no pictures"; exit 1; }
for v in 1 2 h; do [[ "$p0" == "$(pics $v)" ]] || { echo "FAILED: pictures differ with GXV_BLOOM_BRACKET=$v"; diff <(echo "$p0") <(pics $v) || true; exit 1; }; done
for f in 1 6; do
  a=$(scenes 0 $f); b=$(scenes 1 $f); c=$(scenes 2 $f)
  [[ $b -lt $a ]] || { echo "FAILED: frame $f scenes $a -> $b with =1 (expected fewer)"; exit 1; }
  [[ $c -eq $a ]] || { echo "FAILED: frame $f scenes $a -> $c with =2 (expected the same)"; exit 1; }
done
for f in 2 3 4 5 7; do
  [[ $(scenes 0 $f) -eq $(scenes h $f) && $(scenes 0 $f) -eq $(scenes 1 $f) ]] || { echo "FAILED: frame $f scenes changed with =1 (refused brackets must stay)"; exit 1; }
done
[[ $(scenes 0 1) -eq $(scenes h 1) ]] || { echo "FAILED: GXV_HRW=1 changed frame 1's scenes"; exit 1; }
echo "PASS: identical pictures (colour and alpha) with the switch off, on and count-only; frames 1 and 6 scenes $(scenes 0 1)/$(scenes 0 6) -> $(scenes 1 1)/$(scenes 1 6), refused brackets unchanged"

#!/usr/bin/env bash
# Picture-quality test of the present (tests/vgx_present_fast_gl_test.cpp): the real vgx back end on the desktop's GL (Mesa llvmpipe through
# surfaceless EGL) presents a synthetic picture at 720x404 (GXV_WIDE_RUNGS=1 rung 2) and 960x544, in each mode:
#   today      GXV_EDGE_AA=0                      the sharpened present (5 dependent reads) alone; panel 2 = panel 1
#   aa9        GXV_EDGE_AA=1                      PresentEdgeAA (FXAA 3.11 console)
#   fast3      GXV_PRESENT_FAST=1 GXV_EDGE_AA=1   the one-pass present, three reads
#   fast5      GXV_PRESENT_FAST=2 GXV_EDGE_AA=1   the one-pass present, five reads
# and prints the RMS errors against the ideal (8x8 supersampled) picture. Writes the panels (PPM) into the output directory.
#   tools/test_vgx_present_fast_gl.sh [output-dir]
# Needs libEGL / libGLESv2 (Mesa) and the Khronos headers (CPLUS_INCLUDE_PATH). Exit status 0 when every hard check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_present_fast_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$out/vgx_present_fast_gl_test"
common=(GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_WIDE_RUNGS=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0)
status=0
env "${common[@]}" PF_TAG=today GXV_EDGE_AA=0 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
env "${common[@]}" PF_TAG=aa9 GXV_EDGE_AA=1 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
env "${common[@]}" PF_TAG=fast3 GXV_PRESENT_FAST=1 GXV_EDGE_AA=1 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
env "${common[@]}" PF_TAG=fast5 GXV_PRESENT_FAST=2 GXV_EDGE_AA=1 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
# the fast present without edge smoothing (GXV_EDGE_AA off): panel 1 is the one-pass plain present
env "${common[@]}" PF_TAG=fast3plain GXV_PRESENT_FAST=1 GXV_EDGE_AA=0 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
# GXV_EDGE_AA_HUDCUT=1 (exact): the edge pass leaves the HUD rectangles out; every panel must be byte-identical to the run without it,
# with the test's one HUD rectangle and with four more (PF_HUDS=1: overlapping, at the screen's edges, full-width and full-height strips)
same() { if cmp -s "$out/$1" "$out/$2"; then echo "  [ok] $1 = $2"; else echo "  [FAIL] $1 differs from $2"; status=1; fi; }
env "${common[@]}" PF_TAG=hudcut GXV_PRESENT_FAST=1 GXV_EDGE_AA=1 GXV_EDGE_AA_HUDCUT=1 "$out/vgx_present_fast_gl_test" "$out" | grep -v '^\[' || status=1
env "${common[@]}" PF_TAG=fast3h PF_HUDS=1 GXV_PRESENT_FAST=1 GXV_EDGE_AA=1 "$out/vgx_present_fast_gl_test" "$out" > "$out/fast3h.log" 2>&1 || true
env "${common[@]}" PF_TAG=hudcuth PF_HUDS=1 GXV_PRESENT_FAST=1 GXV_EDGE_AA=1 GXV_EDGE_AA_HUDCUT=1 "$out/vgx_present_fast_gl_test" "$out" > "$out/hudcuth.log" 2>&1 || true
echo "GXV_EDGE_AA_HUDCUT=1 against GXV_EDGE_AA_HUDCUT=0:"
for r in 2 6; do
  for p in plain edge; do same "hudcut_r${r}_$p.ppm" "fast3_r${r}_$p.ppm"; same "hudcuth_r${r}_$p.ppm" "fast3h_r${r}_$p.ppm"; done
  if cmp -s "$out/fast3h_r${r}_edge.ppm" "$out/fast3h_r${r}_plain.ppm"; then echo "  [FAIL] PF_HUDS=1 rung $r: no edge smoothing at all"; status=1; fi
done
# GXV_EDGE_AA_NATIVE=0 (not exact, by design): no edge smoothing at 960x544 1:1 (rung 6: the edge panel is the plain one); the upscaled
# rung 2 keeps it (its edge panel is the GXV_EDGE_AA=1 one)
env "${common[@]}" PF_TAG=native0 GXV_PRESENT_FAST=1 GXV_EDGE_AA=1 GXV_EDGE_AA_NATIVE=0 "$out/vgx_present_fast_gl_test" "$out" > "$out/native0.log" 2>&1 || true
echo "GXV_EDGE_AA_NATIVE=0:"
same native0_r6_edge.ppm native0_r6_plain.ppm
same native0_r6_plain.ppm fast3_r6_plain.ppm
same native0_r2_edge.ppm fast3_r2_edge.ppm
exit $status

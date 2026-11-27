#!/usr/bin/env bash
# Test of the upscaler (GXV_UPSCALE; tests/vgx_upscale_gl_test.cpp): the real vgx back end on the desktop's GL (Mesa llvmpipe through
# surfaceless EGL) presents the analytic test scene at the wide rungs 720x404 .. 960x544 and at 832x472, captured as the console shows it
# (960x544; panel 0 = the present without edge smoothing, panel 1 = the full present), with
#   today   GXV_PRESENT_FAST=1 GXV_EDGE_AA=1                (the console's defaults: panel 1 = the edge-smoothed fast present)
#   up      ... GXV_UPSCALE=1                               (panel 1 = the upscaler below 960x544: k 1.5, edge-directed, clamped on edges)
#   up200   ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=200
#   upcr    ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=100 GXV_UPSCALE_CLAMP=0 GXV_UPSCALE_EDGE=0  (plain Catmull-Rom: the cheapest 5-read form)
#   upc0    ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=200 GXV_UPSCALE_CLAMP=0 / upc2 ... CLAMP=2 (k 2: what the clamp does)
#   up3     ... GXV_UPSCALE=1 GXV_UPSCALE_READS=3
#   upne    ... GXV_UPSCALE=1 GXV_UPSCALE_EDGE=0
#   bil     GXV_PRESENT_FAST=0 GXV_EDGE_AA=0 GXV_SHARPEN=off (panel 0 = plain bilinear)
#   up0     ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=0           (the upscaler's weights 0: must be the bilinear picture, clamp and all)
#   split   ... GXV_UPSCALE=1 GXV_UPSCALE_SPLIT=1           (left half = up, right half = today)
#   forced  ... GXV_UPSCALE=2 at 960x544                    (the upscaler at 1:1: the plain picture)
#   upauto  ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=auto        (sharp24: the strength follows the rung: k = 0.75 + 3 (m - 1), 0.75 .. 2.0)
#   uplite0 ... GXV_UPSCALE=1 GXV_UPSCALE_LITE=0           (upscale25: must be byte-identical to up, at every size)
#   uplite1 ... GXV_UPSCALE=1 GXV_UPSCALE_LITE=1           (3 reads at +-(0.75, 0.75), Catmull-Rom lobe, gated clamp)
#   uplite2 ... GXV_UPSCALE=1 GXV_UPSCALE_LITE=2           (sharp bilinear, one dependent read)
#   uplite3 ... GXV_UPSCALE=1 GXV_UPSCALE_LITE=1 GXV_UPSCALE_LITE_CLAMP=0   (3N: the 3-read form without the clamp)
#   upk179  ... GXV_UPSCALE=1 GXV_UPSCALE_SHARP=179         (what auto gives at 720x404: the two must agree within 1 level there)
# Hard checks: at 960x544 1:1 both panels with GXV_UPSCALE=1 are byte-identical to GXV_UPSCALE=0; panel 0 never changes; the split's halves.
# The panels (PPM) stay in the output directory for scoring (agents' upscale_score.py: PSNR / SSIM).
#   tools/test_vgx_upscale_gl.sh [output-dir] [scene.ppm]
# Needs libEGL / libGLESv2 (Mesa) and the Khronos headers (CPLUS_INCLUDE_PATH). Exit status 0 when every hard check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
scene=${2:-syn}
mkdir -p "$out"
src="$root/platform/vgx/src"
bin="$out/vgx_upscale_gl_test"
if [ ! -x "$bin" ]; then
  nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w -I"$src" -I"$root/platform/vgx/third_party" \
    "$root/tests/vgx_upscale_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
    "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$bin"
fi
common=(GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_WIDE_RUNGS=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 PU_SCENE="$scene")
fast=(GXV_PRESENT_FAST=1 GXV_EDGE_AA=1)
status=0
run() { local tag=$1; shift; env "${common[@]}" PU_TAG="$tag" "$@" "$bin" "$out" | grep -v '^\[' || status=1; }
for size in "" 832x472; do
  sz=(); [ -n "$size" ] && sz=(PU_SIZE="$size")
  run today "${sz[@]}" "${fast[@]}"
  run up "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1
  run up200 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_SHARP=200
  run upcr "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_SHARP=100 GXV_UPSCALE_CLAMP=0 GXV_UPSCALE_EDGE=0
  run upc0 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_CLAMP=0 GXV_UPSCALE_SHARP=200
  run upc2 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_CLAMP=2 GXV_UPSCALE_SHARP=200
  run up3 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_READS=3
  run upne "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_EDGE=0
  run bil "${sz[@]}" GXV_PRESENT_FAST=0 GXV_EDGE_AA=0 GXV_SHARPEN=off
  run upauto "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_SHARP=auto
  run uplite0 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_LITE=0
  run uplite1 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_LITE=1
  run uplite2 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_LITE=2
  run uplite3 "${sz[@]}" "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_LITE=1 GXV_UPSCALE_LITE_CLAMP=0
  run up0 "${sz[@]}" GXV_PRESENT_FAST=0 GXV_EDGE_AA=0 GXV_SHARPEN=off GXV_UPSCALE=1 GXV_UPSCALE_SHARP=0
done
run upk179 "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_SHARP=179 PU_RUNGS="2"
run split "${fast[@]}" GXV_UPSCALE=1 GXV_UPSCALE_SPLIT=1 PU_RUNGS="3 6"
run forced "${fast[@]}" GXV_UPSCALE=2 PU_RUNGS="6"
same() { if cmp -s "$out/$1" "$out/$2"; then echo "  [ok] $1 = $2"; else echo "  [FAIL] $1 differs from $2"; status=1; fi; }
differ() { if cmp -s "$out/$1" "$out/$2"; then echo "  [FAIL] $1 = $2 (expected a difference)"; status=1; else echo "  [ok] $1 differs from $2"; fi; }
echo "960x544 1:1: GXV_UPSCALE=1 byte-identical to GXV_UPSCALE=0 (every variant):"
for t in up up200 upcr upc0 upc2 up3 upne upauto; do same "${t}_960x544_p0.ppm" today_960x544_p0.ppm; same "${t}_960x544_p1.ppm" today_960x544_p1.ppm; done
same up0_960x544_p1.ppm bil_960x544_p1.ppm
same split_960x544_p1.ppm today_960x544_p1.ppm
echo "GXV_UPSCALE=2 (forced) at 960x544 1:1: the plain picture (weights 0, no edge smoothing):"
same forced_960x544_p1.ppm bil_960x544_p0.ppm
echo "below 960x544: panel 0 (the present without edge smoothing) unchanged, panel 1 the upscaler:"
for s in 720x404 784x440 848x480 896x512 832x472; do
  same "up_${s}_p0.ppm" "today_${s}_p0.ppm"
  differ "up_${s}_p1.ppm" "today_${s}_p1.ppm"
done
echo "GXV_UPSCALE_LITE (upscale25): 0 = the 5-read upscaler byte for byte; 1 and 2 differ below 960x544, are the plain picture at 1:1:"
for s in 720x404 784x440 848x480 896x512 832x472; do
  same "uplite0_${s}_p1.ppm" "up_${s}_p1.ppm"
  differ "uplite1_${s}_p1.ppm" "up_${s}_p1.ppm"
  differ "uplite2_${s}_p1.ppm" "up_${s}_p1.ppm"
  differ "uplite3_${s}_p1.ppm" "uplite1_${s}_p1.ppm"
  same "uplite1_${s}_p0.ppm" "today_${s}_p0.ppm"
done
for t in uplite0 uplite1 uplite2 uplite3; do same "${t}_960x544_p1.ppm" today_960x544_p1.ppm; done
echo "GXV_UPSCALE_SHARP=auto (sharp24): the strength follows the rung:"
for s in 720x404 784x440 848x480 896x512; do differ "upauto_${s}_p1.ppm" "up_${s}_p1.ppm"; done
python3 - "$out" <<'PY' || status=1
import sys
def rd(p):
    with open(p, 'rb') as f:
        d = f.read()
    hdr = d.split(b'\n', 3)
    return d[len(b'\n'.join(hdr[:3])) + 1:]
o = sys.argv[1]
a, b = rd(o + '/upauto_720x404_p1.ppm'), rd(o + '/upk179_720x404_p1.ppm')
w = max(abs(x - y) for x, y in zip(a, b))
print('  [%s] auto at 720x404 (k = 1.7896) within %d level(s) of the explicit k = 1.79 (<= 1)' % ('ok' if w <= 1 else 'FAIL', w))
sys.exit(0 if w <= 1 else 1)
PY
echo "GXV_UPSCALE_SPLIT=1 (784x440): left half = the upscaler (within 1 level: the half-width quad interpolates its coordinates from other"
echo "vertices, so the phase's last bits can differ), right half = today's present (exact):"
python3 - "$out" <<'PY' || status=1
import sys
def rd(p):
    with open(p, 'rb') as f:
        d = f.read()
    hdr = d.split(b'\n', 3)  # P6 / 960 544 / 255 / pixels
    return d[len(b'\n'.join(hdr[:3])) + 1:]
o = sys.argv[1]
sp, up, td = rd(o + '/split_784x440_p1.ppm'), rd(o + '/up_784x440_p1.ppm'), rd(o + '/today_784x440_p1.ppm')
W, H = 960, 544
left = right = 0
for y in range(H):
    a = y * W * 3
    left = max(left, max(abs(p - q) for p, q in zip(sp[a:a + 480 * 3], up[a:a + 480 * 3])))
    right += sp[a + 480 * 3:a + W * 3] != td[a + 480 * 3:a + W * 3]
ok = left <= 1 and right == 0
print('  [%s] split: left half worst difference from up %d level(s) (<= 1), right half rows differing from today %d (0)' % ('ok' if ok else 'FAIL', left, right))
sys.exit(0 if ok else 1)
PY
exit $status

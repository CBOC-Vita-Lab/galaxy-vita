#!/usr/bin/env bash
# aa25 host test (tests/vgx_aa25_gl_test.cpp): the depth-gated edge pass GXV_EDGE_DEPTH on the real vgx back end (Mesa llvmpipe, surfaceless
# EGL) at 960x544 1:1, against a 4x4 supersampled reference of an analytic scene with real depth; plus the GXV_MSAA_PROBE triangle at rung 6
# (hard edges) and rung 8 (the desktop's multisampled EFB: intermediate greys).
#   tools/test_vgx_aa25_gl.sh [output-dir]        SAN=1: built with -fsanitize=address,undefined
# Needs libEGL / libGLESv2 (Mesa) and the Khronos headers (CPLUS_INCLUDE_PATH). Exit status 0 when every hard check passes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
bin="$out/vgx_aa25_gl_test"
san=(); [ "${SAN:-0}" = 1 ] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
if [ ! -x "$bin" ]; then
  nice -n 10 "${CXX:-g++}" -std=gnu++20 -O1 -g -w "${san[@]}" -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
    -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" \
    "$root/tests/vgx_aa25_gl_test.cpp" "$src/vgx_backend_gl.cpp" "$src/vgx_autoq.cpp" "$src/vgx_shadergen.cpp" \
    "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -lpthread -o "$bin"
fi
common=(GXV_CAP_PRESENT=960x544 GXV_CAP_AB=1 GXV_WIDE_RUNGS=1 LIBGL_ALWAYS_SOFTWARE=1 GXV_LOG=0 GXV_PRESENT_FAST=1 ASAN_OPTIONS=detect_leaks=0)
status=0
run() { local tag=$1; shift; env "${common[@]}" PU_TAG="$tag" "$@" "$bin" "$out" | grep -E "FAIL|checks|\[aa25\]" || status=1; }
run plain PU_REFS=1 GXV_EDGE_AA=0
run edge5 GXV_EDGE_AA=1
run ez0 GXV_EDGE_AA=1 GXV_EDGE_DEPTH=0
run ez1 GXV_EDGE_AA=1 GXV_EDGE_DEPTH=1
run ez2 GXV_EDGE_AA=1 GXV_EDGE_DEPTH=2
run ez3 GXV_EDGE_AA=1 GXV_EDGE_DEPTH=3
run probe6 GXV_EDGE_AA=0 GXV_MSAA_PROBE=1 PU_RUNG=6 # (no edge pass: it would smooth the probe at 1:1)
run probe8 GXV_EDGE_AA=0 GXV_MSAA_PROBE=1 GXV_AUTOQ_MSAA=1 PU_RUNG=8 || true # (its colour upload is refused by the desktop's multisampled object: only the probe counts)
python3 - "$out" <<'PY' || status=1
import sys, numpy as np
from PIL import Image
d = sys.argv[1]
L = lambda n: np.asarray(Image.open(f"{d}/{n}")).astype(np.float64)
ref, one, m = L("ref_ss.ppm"), L("ref_1x.ppm"), L("aa25_mask.pgm") > 0
inner = ~m; inner[:2, :] = inner[-2:, :] = False; inner[:, :2] = inner[:, -2:] = False
fail = 0
def ok(c, what):
    global fail
    print("  [%s] %s" % ("ok" if c else "FAIL", what)); fail += 0 if c else 1
def psnr(a, b, k=None):
    e = (a - b) ** 2; e = e[k] if k is not None else e
    return 10 * np.log10(255 ** 2 / e.mean())
ok((L("plain_p1.ppm") == one).all(), "no edge pass: the 1x picture exactly")
ok((L("ez0_p1.ppm") == L("edge5_p1.ppm")).all(), "GXV_EDGE_DEPTH=0: byte-identical to edge5")
for n in ("edge5", "ez1", "ez2"):
    a = L(n + "_p1.ppm"); ch = (np.abs(a - one).max(axis=2) > 2)[inner].mean() * 100
    print("  %-6s PSNR %.2f, edge band %.2f dB, interior pixels changed %.2f %%" % (n, psnr(a, ref), psnr(a, ref, m), ch))
e5, z1, z2 = L("edge5_p1.ppm"), L("ez1_p1.ppm"), L("ez2_p1.ppm")
ok(psnr(z1, ref, m) >= psnr(e5, ref, m), "depth-gated: geometry edges at least as close to the reference as edge5")
ok((np.abs(z1 - one).max(axis=2) > 2)[inner].mean() < 0.2 * (np.abs(e5 - one).max(axis=2) > 2)[inner].mean(), "depth-gated: interior (texture) pixels changed < 1/5 of edge5's")
ok(np.abs(z1 - z2).max() <= 2, "branch form = no-branch form (within 2 levels)")
p6, p8 = L("probe6_p1.ppm").mean(axis=2)[:64, :64], L("probe8_p1.ppm").mean(axis=2)[:64, :64]
i6, i8 = ((p6 > 20) & (p6 < 235)).sum(), ((p8 > 20) & (p8 < 235)).sum()
ok((p6 >= 235).sum() > 500 and i6 == 0, "GXV_MSAA_PROBE at rung 6: the triangle, hard edges (%d intermediate)" % i6)
ok(i8 > 40, "GXV_MSAA_PROBE at rung 8 (desktop multisampled EFB): %d intermediate pixels" % i8)
sys.exit(1 if fail else 0)
PY
echo "aa25: $([ $status = 0 ] && echo all checks passed || echo SOME CHECKS FAILED)"
exit $status

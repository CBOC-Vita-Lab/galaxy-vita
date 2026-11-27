#!/usr/bin/env bash
# GXV_AUTOMIP (automip25): mip levels for textures the game declares without any. The production GX front end, texture cache and GLES back end
# on Mesa (tests/vgx_automip_gl.cpp: a road, a CMPR wall, an alpha fence, a nearest strip, small / changing textures and a 2D picture, the camera
# panning), no game data, no Vita. Checks:
#   - the picture of every frame is bit-identical with GXV_AUTOMIP unset, 0 and 2 (census), with the budget at 0, and (against a tree without
#     this change: AUTOMIP_BASE=<root of that tree>) with the switch unset;
#   - with =1 the pictures differ below the horizon (far road) but not in the rows near the camera and not in the corner (the 2D picture: given a
#     chain at its upload, then taken back, the entry decoded again the plain way);
#   - the temporal-stability number (mean absolute difference of consecutive pictures, from picture 40 on) of the far road falls;
#   - the pure parts (tests/vgx_automip_chain.cpp): the coverage-preserving alpha and the chain's cost on the host.
# Prints the numbers and writes PPMs of pictures 30 and 100 into the output directory.
#   tools/test_vgx_automip_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include; AM_SKIP_BUILD=1 reuses the binaries)
set -euo pipefail
root=${AM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 -w -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr")
if [[ "${AM_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"
    if [[ ! -f "$o" || "$src/$unit.cpp" -nt "$o" || "$src/vgx_cmd.h" -nt "$o" || "$src/vgx_automip.h" -nt "$o" ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_automip_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_automip_gl"
  "${CXX:-g++}" -std=c++20 -O1 -w -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" "$root/tests/vgx_automip_chain.cpp" "$src/vgx_texdecode.cpp" -o "$out/vgx_automip_chain"
fi
fails=0
run() { # <tag> <binary> env assignments...
  local tag=$1 bin=$2; shift 2
  env -i PATH="$PATH" HOME=/tmp LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_EFBSWAP=0 GXV_TEXMEMO=0 \
    AM_FRAMES=100 AM_DUMP="$out/$tag" "$@" "$bin" > "$out/$tag.full" 2>&1 || true
  grep -q "FAIL" "$out/$tag.full" && { echo "FAILED: $tag"; grep FAIL "$out/$tag.full" | head -3; fails=$((fails + 1)); }
  grep -E '^frame' "$out/$tag.full" | sed 's/frame [0-9]* hash //' > "$out/$tag.hash"
  grep -E '^stab' "$out/$tag.full" > "$out/$tag.stab"
  grep -E 'automip' "$out/$tag.full" | tail -3 > "$out/$tag.log" || true
}
same() { cmp -s "$out/$1.hash" "$out/$2.hash" && echo "  same pictures: $1 = $2" || { echo "MISMATCH: $2 differs from $1"; fails=$((fails + 1)); }; }
bin="$out/vgx_automip_gl"
run unset "$bin"
run zero "$bin" GXV_AUTOMIP=0
run census "$bin" GXV_AUTOMIP=2 GXV_AUTOMIP_SEC=1
run budget0 "$bin" GXV_AUTOMIP=1 GXV_AUTOMIP_KB=0
vita="GXV_MIPGEN=1 GXV_MIPCLAMP=1 GXV_AUTOMIP_DXT_PLAY=1"
run vita_off "$bin" $vita
run on "$bin" GXV_AUTOMIP=1 GXV_AUTOMIP_SEC=1
run vita_on "$bin" $vita GXV_AUTOMIP=1 GXV_AUTOMIP_SEC=1
run vita_tri "$bin" $vita GXV_AUTOMIP=1 GXV_AUTOMIP_FILTER=2 GXV_AUTOMIP_SEC=1
run alpha1 "$bin" $vita GXV_AUTOMIP=1 GXV_AUTOMIP_ALPHA=1 GXV_AUTOMIP_SEC=1
echo "default path:"
same unset zero; same unset census; same unset budget0; same unset vita_off
if [[ -n "${AM_BASE:-}" && -x "$AM_BASE" ]]; then
  run base "$AM_BASE"; same base unset
fi
[[ $(cat "$out/census.log" | grep -c automip-census) -gt 0 ]] || { echo "no census line"; fails=$((fails + 1)); }
for t in on vita_on; do cmp -s "$out/unset.hash" "$out/$t.hash" && { echo "FAILED: $t made no difference"; fails=$((fails + 1)); }; done
python3 - "$out" <<'PY' || fails=$((fails + 1))
import sys
from PIL import Image
import numpy as np
out = sys.argv[1]
def load(n): return np.array(Image.open(f'{out}/{n}-f100.ppm').convert('RGB')).astype(int)
def stab(n): return {l.split()[1]: float(l.split()[2]) for l in open(f'{out}/{n}.stab')}
ok = True
base = load('unset')
for n in ('on', 'vita_on', 'vita_tri', 'alpha1'):
    d = abs(load(n) - base).sum(axis=2)
    corner, near, far = d[22:90, 544:608], d[380:456, :], d[232:300, 200:440]
    print(f'{n}: corner (2D picture) max diff {corner.max()}, rows near the camera max diff {near.max()}, far road mean diff {far.mean():.1f}')
    if corner.max() != 0 or near.max() != 0 or far.mean() < 1: ok = False
s0 = stab('unset')
print('temporal stability, mean absolute difference of consecutive pictures (lower is steadier):')
print('  %-9s %9s %9s %9s %9s' % ('', 'whole', 'far-road', 'near-road', 'wall-band'))
for n in ('unset', 'on', 'vita_on', 'vita_tri', 'alpha1'):
    s = stab(n)
    print('  %-9s %9.3f %9.3f %9.3f %9.3f' % (n, s['whole'], s['far-road'], s['near-road'], s['wall-band']))
    if n != 'unset' and not (s['far-road'] < s0['far-road'] and abs(s['near-road'] - s0['near-road']) < 1e-9): ok = False
sys.exit(0 if ok else 1)
PY
echo "log lines:"; for t in census vita_on alpha1; do sed 's/^/  /' "$out/$t.log" | cut -c1-420; done
echo "pure parts:"; "$out/vgx_automip_chain" || fails=$((fails + 1))
[[ $fails == 0 ]] && echo "ALL CHECKS PASSED" || { echo "$fails FAILED"; exit 1; }

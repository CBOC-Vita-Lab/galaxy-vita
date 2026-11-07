#!/usr/bin/env bash
# GXV_SHADOWFILL_BOUNDS (vgx_draw.cpp, "shadow fill bounds"): the production GX front end and GLES back end on Mesa
# (tests/vgx_shadowfill_gl.cpp). At each render size (the EFB's 128x128, 160x120 and 96x96: scales above and below 1, not whole) the
# pictures and the alpha capture's texture (every channel) must be identical with the switch on and off, GXV_PASSCHECK=1's pixel
# model must raise no failure, and with the switch on the fills must have been left out / scissored as the test expects.
#   tools/test_vgx_shadowfill_gl.sh [output-dir]   (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /usr/include)
#   Objects are kept in <output-dir>/obj (only changed units are built again); SF_SKIP_BUILD=1 reuses the binary.
set -euo pipefail
root=${SF_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/usr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 ${SF_CXXFLAGS:-} -I"$src" -I"$root/platform/vgx/third_party" -I"$khr")
if [[ "${SF_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_shadowfill_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfill_gl"
fi
# Two profiles: the plain one (without the CPU transform, so that the test's display lists become static buffers), and the console's picture defaults (vita/vgx-game/entry.cpp: constant destination alpha under blending,
# the shader trims and half precision, the pixel-work cuts, ...), under which the fill writes its alpha as the constant 0.
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
rm -f "$out"/sf-*.log
fail=0
for profile in plain vita; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  for size in "128 128" "160 120" "96 96"; do
    set -- $size
    ref=""
    for sw in 0 1; do
      tag="$profile-$1x$2-$sw"
      env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_PASSCHECK=1 $penv \
        SF_RW=$1 SF_RH=$2 GXV_SHADOWFILL_BOUNDS=$sw "$out/vgx_shadowfill_gl" > "$out/sf-$tag.log" 2>&1 || fail=1
      grep -v "^libunwind" "$out/sf-$tag.log" | grep -E "FAIL|^cuts|PASSCHECK" | sort -u | sed "s/^/$tag: /" || true
      h=$(grep -E '^frame[0-9]+ hash' "$out/sf-$tag.log" | tr '\n' ' ')
      if [[ -z "$ref" ]]; then ref="$h"; echo "$tag: $h"
      elif [[ "$h" != "$ref" ]]; then echo "FAILED: $tag pictures differ from the switch off:"; echo "$h"; fail=1; fi
    done
  done
done
grep -l -E "FAIL|PASSCHECK FAILED" "$out"/sf-*.log && fail=1
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: identical pictures and alpha captures with GXV_SHADOWFILL_BOUNDS on and off, cuts made"

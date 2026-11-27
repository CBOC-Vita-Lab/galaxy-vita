#!/usr/bin/env bash
# shadow25: GXV_SILH_GATE (vgx_draw.cpp SilhGatePass) through the production front end and GLES back end on Mesa (tests/vgx_shadowfill_gl.cpp, SF_SILH=1,
# frames 40-46, the game's frame with SF_TRIPLE=1). Frame 40 nothing in the alpha; 41 silhouette visible, hint 2; 42 silhouette with no pixel, hint 1; 43 silhouette
# visible, hint 1 (the false negative: the picture MUST differ); 44 no pixel, hint 2; 45 visible, no hint; 46 volume + silhouette, hint 1.
#   tools/test_vgx_silhgate25_gl.sh [output-dir]   (KHR_INCLUDE, default /work/khr/include; SG_OBJ=<dir with built objects> to reuse them)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
flags=(-std=c++20 -O1 -g0 -w -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr")
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texcomp vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
objs=()
if [[ "${SG_SKIP_BUILD:-0}" != 1 ]]; then
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"; objs+=("$o")
    if [[ ! -f "$o" || -n "$(find "$src/$unit.cpp" "$src"/*.h "$src"/*.inc "$root/platform/include/port" -newer "$o" 2>/dev/null | head -1)" ]]; then
      nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o" &
      while [[ $(jobs -r | wc -l) -ge ${JOBS:-2} ]]; do sleep 1; done
    fi
  done
  wait
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_shadowfill_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "$out"/obj/*.o -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfill_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1 GXV_PX_NOOP=1"
E="LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_TEXMEMO=0 SF_TRIPLE=1 SF_SILH=1 $vita"
run() { local tag=$1; shift; env $E "$@" "$out/vgx_shadowfill_gl" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true; grep -E '^(frame|FAIL)' "$out/$tag.log" > "$out/$tag.hash" || true; }
fail=0
run off GXV_SILH_GATE=0
run on  GXV_SILH_GATE=1
run cen GXV_SILH_GATE=2
for t in off on cen; do grep -q FAIL "$out/$t.log" && { grep -H FAIL "$out/$t.log" | head -3; fail=1; }; done
echo "== scenes a frame (off / on / census)"; paste <(sed -E 's/ hash=[0-9a-f]+//' "$out/off.hash") <(sed -E 's/frame[0-9]+ hash=[0-9a-f]+//' "$out/on.hash") <(sed -E 's/frame[0-9]+ hash=[0-9a-f]+//' "$out/cen.hash")
differ() { diff <(sed -E 's/ scenes=[0-9]+//' "$out/off.hash") <(sed -E 's/ scenes=[0-9]+//' "$out/$1.hash") | grep '^>' | sed -E 's/^> (frame[0-9]+).*/\1/' | tr '\n' ' '; }
echo "frames whose picture differs, switch on:     [$(differ on)]   (expected: frame43 only)"
echo "frames whose picture differs, census (=2):   [$(differ cen)]   (expected: none)"
grep -h silhgate "$out/on.log" | tail -1
[[ "$(differ on)" == "frame43 " ]] || { echo "FAILED: on-differences"; fail=1; }
[[ -z "$(differ cen)" ]] || { echo "FAILED: census changed a picture"; fail=1; }
[[ $fail == 0 ]] && echo "ok: silhgate25" || exit 1

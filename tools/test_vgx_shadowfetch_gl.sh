#!/usr/bin/env bash
# GXV_SHADOWFETCH (gpu23b): the silhouette fill reading the EFB's alpha in its shader, through the production front end and GLES back
# end on Mesa (tests/vgx_shadowfetch_gl.cpp = tests/vgx_shadowfill_gl.cpp's scene + counters). For each render size and both
# GXV_SHADOWFILL_BOUNDS settings the pictures (all four channels) must be identical with GXV_SHADOWFETCH=0, 2 and 1; with 1 the fills
# after the calibration must have been fetched (the capture left out), and no stale read may be counted. Needs Mesa's
# The desktop has no usable frame read (llvmpipe's gl_LastFragData reads 0): the back end stands in with a copy of the target taken
# just before the fetch quad; the calibration, the fitted arithmetic and the pairing are the console's. The plain profile's fill writes
# an alpha that depends on the pixel (no GXV_FIX_DSTA): never paired, it only shows the switch changes nothing there.
#   tools/test_vgx_shadowfetch_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include; SFE_SKIP_BUILD=1 reuses)
set -euo pipefail
root=${SFE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 ${SFE_CXXFLAGS:-} -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" -I"$khr")
if [[ "${SFE_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_shadowfetch_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfetch_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
rm -f "$out"/sfe-*.log
fail=0
for profile in plain vita; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  for size in ${SFE_SIZES:-"128x128" "160x120" "96x96"}; do
    rw=${size%x*}; rh=${size#*x}
    for bounds in 0 1; do
      ref=""
      for sw in 0 2 1; do
        tag="$profile-$size-b$bounds-f$sw"
        env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $penv \
          SF_RW=$rw SF_RH=$rh GXV_SHADOWFILL_BOUNDS=$bounds GXV_SHADOWFETCH=$sw "$out/vgx_shadowfetch_gl" > "$out/sfe-$tag.log" 2>&1 || true
        h=$(grep -E '^frame[0-9]+ hash' "$out/sfe-$tag.log" | sed -E 's/ alpha=.*//' | tr '\n' ' ')
        counts=$(grep -E '^shadowfetch pictures' "$out/sfe-$tag.log" || true)
        cal=$(grep -E 'GXV_SHADOWFETCH: key' "$out/sfe-$tag.log" | head -2 | sed 's/^/    /' || true)
        echo "$tag: $counts"; [[ -n "$cal" ]] && echo "$cal"
        # (with the copy left out the alpha capture's texture keeps an older picture: the scene's checks that read that texture, or count
        # the captures made fills of 0 - one fewer is needed when the texture still holds the 0 of an earlier fill - do not apply)
        allowed="the volumes put alpha on the screen|capture made a fill of 0"
        if [[ $sw == 1 ]]; then bad=$(grep -E "^FAIL" "$out/sfe-$tag.log" | grep -v -E "$allowed" || true)
        else bad=$(grep -E "^FAIL" "$out/sfe-$tag.log" || true); fi
        [[ -n "$bad" ]] && { echo "$bad" | sed "s/^/$tag: /"; fail=1; }
        if [[ -z "$ref" ]]; then ref="$h"
        elif [[ "$h" != "$ref" ]]; then echo "FAILED: $tag pictures differ from GXV_SHADOWFETCH=0:"; echo "  0: $ref"; echo "  $sw: $h"; fail=1; fi
        sc=$(grep -E '^scenes total' "$out/sfe-$tag.log" | sed -E 's/.*=//')
        [[ $sw == 0 ]] && s0=$sc
        echo "    scenes $sc"
        if [[ $sw == 1 ]]; then
          if [[ $profile == vita ]]; then [[ "${sc:-0}" -lt "${s0:-0}" ]] || { echo "FAILED: $tag: scenes $s0 -> $sc (expected fewer)"; fail=1; }; fi
          fetched=$(echo "$counts" | sed -E 's/.*fetched=([0-9]+).*/\1/'); stale=$(echo "$counts" | sed -E 's/.*stale=([0-9]+).*/\1/')
          if [[ $profile == vita ]]; then [[ "${fetched:-0}" -gt 0 ]] || { echo "FAILED: $tag: no fill was fetched"; fail=1; }; fi
          [[ "${stale:-1}" -eq 0 ]] || { echo "FAILED: $tag: stale reads"; fail=1; }
        fi
      done
    done
  done
done
# Controls: a calibration made wrong on purpose (GXV_SHADOWFETCH_BREAK=1 the red formula one off, =2 the threshold one up, =3 almost every
# pixel wrong - the console's c3 case) must be refused (no fill fetched, the probes logged) and the pictures must stay those of
# GXV_SHADOWFETCH=0. And no printf "%z" in the back end or front end: the Vita's newlib has no 'z' length modifier (c3 crashed in
# strlen on "%zu ... %s").
for brk in 1 2 3; do
  tag="control-break$brk"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $vita \
    SF_RW=160 SF_RH=120 GXV_SHADOWFILL_BOUNDS=1 GXV_SHADOWFETCH=1 GXV_SHADOWFETCH_BREAK=$brk "$out/vgx_shadowfetch_gl" > "$out/sfe-$tag.log" 2>&1 || true
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $vita \
    SF_RW=160 SF_RH=120 GXV_SHADOWFILL_BOUNDS=1 GXV_SHADOWFETCH=0 "$out/vgx_shadowfetch_gl" > "$out/sfe-$tag-ref.log" 2>&1 || true
  echo "$tag: $(grep -E '^shadowfetch pictures' "$out/sfe-$tag.log")"; grep -E 'GXV_SHADOWFETCH: key' "$out/sfe-$tag.log" | head -1 | cut -c1-220 | sed 's/^/    /'
  grep -q "NOT exact" "$out/sfe-$tag.log" || { echo "FAILED: $tag: the broken calibration was not refused"; fail=1; }
  grep -q "fetched=0 " "$out/sfe-$tag.log" || { echo "FAILED: $tag: a fill was fetched after a refused calibration"; fail=1; }
  [[ "$(grep -E '^frame[0-9]+ hash' "$out/sfe-$tag.log" | sed -E 's/ alpha=.*//')" == "$(grep -E '^frame[0-9]+ hash' "$out/sfe-$tag-ref.log" | sed -E 's/ alpha=.*//')" ]] ||
    { echo "FAILED: $tag: pictures differ"; fail=1; }
done
# The fallback (a fetched pair whose fill cannot be fetched when it comes: the copy is made at the fill), every time: same pictures.
tag="control-fallback"
env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $vita \
  SF_RW=160 SF_RH=120 GXV_SHADOWFILL_BOUNDS=1 GXV_SHADOWFETCH=1 GXV_SHADOWFETCH_FALLBACK=1 "$out/vgx_shadowfetch_gl" > "$out/sfe-$tag.log" 2>&1 || true
echo "$tag: $(grep -E '^shadowfetch pictures' "$out/sfe-$tag.log")"
[[ "$(grep -E '^frame[0-9]+ hash' "$out/sfe-$tag.log" | sed -E 's/ alpha=.*//')" == "$(grep -E '^frame[0-9]+ hash' "$out/sfe-control-break1-ref.log" | sed -E 's/ alpha=.*//')" ]] ||
  { echo "FAILED: $tag: pictures differ"; fail=1; }
if grep -n '"[^"]*%z[uidx]' "$src/vgx_backend_gl.cpp" "$src/vgx_draw.cpp"; then echo "FAILED: a %z printf format (the Vita has none)"; fail=1; fi
for brk in 1 2 3; do grep -q "GXV_SHADOWFETCH: probes over" "$out/sfe-control-break$brk.log" || { echo "FAILED: control-break$brk: no probe line"; fail=1; }; done
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: identical pictures with GXV_SHADOWFETCH 0 / 2 / 1, fills fetched, no stale read"

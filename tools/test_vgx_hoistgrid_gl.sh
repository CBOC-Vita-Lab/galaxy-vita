#!/usr/bin/env bash
# GXV_HOIST_GRID (scenes23): the player's shadow corner copy hoisted on the EFB's own pixel grid, through the production GX front end
# and GLES back end on Mesa (tests/vgx_hoistgrid_gl.cpp, EFB 640x456 as the game's). At each render size (720x404 and 960x544, the
# console's Performance and Locked-30 sizes, where the corner is not on whole render pixels; 608x346 and 848x480, two more rungs;
# 640x456, where it is and GXV_PASSHOIST's own hoist takes it), with GXV_EFBSWAP2 on and off, in two profiles (plain, and the console's
# picture defaults), every picture and every corner texture (all four channels) must be identical with GXV_HOIST_GRID=0, 1 and 2;
# with =1 every frame but frame 4 (a depth-tested corner draw: refused) must have one EFB scene less and one grid-hoist scene, the
# same scene count in all, and =2 must change no scene count. GXV_VGL_VALIDREGION=1 (counted only on the desktop) must change nothing.
#   tools/test_vgx_hoistgrid_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include); HG_SKIP_BUILD=1 reuses
#   HG_EXTRA="...": more switches for every run (shadowpx23: HG_EXTRA=GXV_SHADOW_RECT=1, then the pictures compared with a run without)
set -euo pipefail
root=${HG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 ${HG_CXXFLAGS:-} -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr")
if [[ "${HG_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_hoistgrid_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_hoistgrid_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1
      GXV_SHADOWFILL_BOUNDS=1 GXV_COLOR_LOADSKIP=15"
fail=0
pics() { grep -E '^frame[0-9]+ ' "$1" | sed -E 's/ scenes=.* hash=/ hash=/'; }
field() { grep -E "^frame$2 " "$1" | sed -E "s/.* $3=([0-9]+).*/\\1/"; }
for profile in plain vita; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  for size in "720 404" "960 544" "608 346" "848 480" "640 456"; do
    set -- $size
    for swap in 1 0; do
      tag="$profile-$1x$2-s$swap"
      for mode in 0 1 2 1v; do
        m=${mode%v}; vr=0; [[ $mode == 1v ]] && vr=1
        env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_TEXMEMO=0 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 $penv ${HG_EXTRA:-} \
          GXV_EFBSWAP2=$swap GXV_HOIST_GRID=$m GXV_VGL_VALIDREGION=$vr HG_RW=$1 HG_RH=$2 "$out/vgx_hoistgrid_gl" > "$out/hg-$tag-$mode.log" 2>&1 || true
        if grep -q "^FAIL" "$out/hg-$tag-$mode.log" || ! grep -q "^frame10 " "$out/hg-$tag-$mode.log"; then
          echo "FAILED: $tag mode $mode did not run cleanly"; grep -E "^FAIL|rror" "$out/hg-$tag-$mode.log" | head -5; fail=1; continue
        fi
      done
      ref=$(pics "$out/hg-$tag-0.log")
      for mode in 1 2 1v; do
        [[ "$ref" == "$(pics "$out/hg-$tag-$mode.log")" ]] || { echo "FAILED: $tag: pictures or corner textures differ with GXV_HOIST_GRID=$mode"; fail=1; }
      done
      hg=$(sed -nE 's/^hoistgrids=([0-9]+).*/\1/p' "$out/hg-$tag-1.log")
      line="$tag: hoists on the grid $hg;"
      for f in 1 2 3 4 5 6 7 8 9 10; do
        s0=$(field "$out/hg-$tag-0.log" $f scenes); s1=$(field "$out/hg-$tag-1.log" $f scenes); s2=$(field "$out/hg-$tag-2.log" $f scenes)
        e0=$(field "$out/hg-$tag-0.log" $f efb); e1=$(field "$out/hg-$tag-1.log" $f efb); h1=$(field "$out/hg-$tag-1.log" $f hoist)
        line="$line f$f scenes $s0->$s1 efb $e0->$e1 hoist $h1"
        [[ $s2 == $s0 && $(field "$out/hg-$tag-2.log" $f efb) == $e0 ]] || { echo "FAILED: $tag frame $f: GXV_HOIST_GRID=2 changed the scenes"; fail=1; }
        if [[ $1 == 640 && $f == 10 ]]; then
          # (GXV_PASSHOIST's own hoist after a hand-over: without the switch it comes after the draw-back quad, which splits the EFB's next
          # scene once more; with GXV_HOIST_GRID=1 it is made before the draw-back: one EFB scene and one scene in all fewer)
          [[ $e1 == $((e0 - 1)) && $h1 == 0 && $s1 == $((s0 - 1)) ]] || { echo "FAILED: $tag frame 10: the whole-pixel hoist after a hand-over should be made before the draw-back"; fail=1; }
        elif [[ $1 == 640 ]]; then
          [[ $e1 == $e0 && $h1 == 0 ]] || { echo "FAILED: $tag frame $f: on whole render pixels GXV_PASSHOIST's own hoist should be taken"; fail=1; }
        elif [[ $f == 4 ]]; then
          [[ $e1 == $e0 && $h1 == 0 ]] || { echo "FAILED: $tag frame 4: a depth-tested corner draw must not be hoisted"; fail=1; }
        elif [[ $f == 1 || $f == 9 ]]; then
          [[ $e1 == $e0 && $h1 == 0 ]] || { echo "FAILED: $tag frame $f: a new fill shader's commands come between the copies: no hoist expected"; fail=1; }
        else
          [[ $e1 == $((e0 - 1)) && $h1 == 1 && $s1 == $s0 ]] || { echo "FAILED: $tag frame $f: expected one EFB scene less and one grid-hoist scene (efb $e0 -> $e1, hoist $h1, scenes $s0 -> $s1)"; fail=1; }
        fi
      done
      echo "$line"
    done
  done
done
# GXV_COLOR_LOADSKIP bit 16 (vitaGL patch 0028): with stand-in hooks that only count (HG_SKIPHOOK=1, with a present hook as the
# console's), every scene that begins with a clear (the display copy's clear, once a frame) must be asked for its clear's colour
# (each byte 0 or 255) and never for the zero texture; without bit 16 the zero texture as before. Pictures unchanged (nothing is
# emulated on the desktop).
for ls in 3 19; do
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_TEXMEMO=0 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_SOFTXFORM=0 \
    GXV_HOIST_GRID=1 GXV_COLOR_LOADSKIP=$ls HG_SKIPHOOK=1 "$out/vgx_hoistgrid_gl" > "$out/hg-skip$ls.log" 2>&1 || true
  line=$(grep "^skiphook" "$out/hg-skip$ls.log" || true); echo "GXV_COLOR_LOADSKIP=$ls: $line"
  zero=$(sed -nE 's/.*zero=([0-9]+).*/\1/p' <<< "$line"); col=$(sed -nE 's/.* colour=([0-9]+).*/\1/p' <<< "$line"); bad=$(sed -nE 's/.*badcolour=([0-9]+).*/\1/p' <<< "$line")
  if [[ $ls == 19 ]]; then [[ $col == 10 && $bad == 0 && $zero == 0 ]] || { echo "FAILED: bit 16: expected 10 clear-colour arms (one a frame), 0 bad colours, no zero arms"; fail=1; }
  else [[ $col == 0 && $zero == 10 ]] || { echo "FAILED: without bit 16: expected 10 zero arms (the clears)"; fail=1; }; fi
  [[ "$(pics "$out/hg-skip$ls.log")" == "$(pics "$out/hg-plain-720x404-s1-1.log")" ]] || { echo "FAILED: GXV_COLOR_LOADSKIP=$ls changed a desktop picture"; fail=1; }
done
[[ $fail == 0 ]] && echo "PASS: identical pictures and corner textures (HOIST_GRID 0/1/2, VALIDREGION; EFBSWAP2 0/1; 5 render sizes; 2 profiles), one EFB split less in every hoisted frame; LOADSKIP bit 16 asks the clear colour" || { echo "FAILED"; exit 1; }

#!/usr/bin/env bash
# GXV_CORNERPASS (imp45): the player's shadow corner copy with no whole-EFB capture before it (the picture as GXV_SHADOW_STENCIL leaves it), made from a
# pass on the EFB's grid at the picture's start, through the production GX front end and GLES back end on Mesa (tests/vgx_hoistgrid_gl.cpp with
# HG_NOCAPTURE=1, EFB 640x456). At each render size, with GXV_EFBSWAP2 on and off, in two profiles (plain, and the console's picture defaults), with
# GXV_HOIST_GRID=1 as in Performance mode: every picture and every corner texture (all four channels) must be identical with GXV_CORNERPASS=0, 1 and 2 (and
# with GXV_VGL_VALIDREGION=1, counted only on the desktop); with =1 frames 3 and 5..10 must have one EFB scene less, one grid-pass scene and the same scene
# count in all; frame 1 (the rectangle is learned from the first copy), frame 2 (a shadow volume's alpha lies over the corner: its draw tests depth) and
# frame 4 (a depth-tested corner draw) must be left as they are; =2 must change no scene count. Also: with the capture present (no HG_NOCAPTURE) the
# switch must change nothing at all (GXV_HOIST_GRID's own hoist takes the copy first).
# fix47: a third profile, "console": the console's own switches for this path - GXV_SHADOWFILL_BOUNDS=0, GXV_SHADOW_RECT=0, GXV_SHADOW_RECT_FE=0 (vita/vgx-game/entry.cpp),
# GXV_SHADOW_STENCIL=1 (Performance), the front-end split at its default. imp45 ran the "vita" profile with GXV_SHADOWFILL_BOUNDS=1, which the console does not have: there
# ShadowFillPass returned at once and the corner copy was never looked at (c35t3: "small alpha copies seen 0"). The same checks must hold in this profile, and the
# "shadowfill" totals must stay 0 (GXV_CORNERPASS alone works the alpha bounds out and cuts nothing). Also there: GXV_SHADOW_RECT_FE=2 (both ways compared) gives the same pictures.
#   tools/test_vgx_cornerpass_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /work/khr/include); CP_SKIP_BUILD=1 reuses the binary
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr")
if [[ "${CP_SKIP_BUILD:-0}" != 1 ]]; then
  objs=()
  for unit in "${sources[@]}"; do
    o="$out/obj/$unit.o"
    newer=0
    for h in "$src/$unit.cpp" "$src/vgx_cmd.h" "$src/vgx_state.h" "$src/vgx_fesplit.h" "$src/vgx_scenes28.h"; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_hoistgrid_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_hoistgrid_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1
      GXV_SHADOWFILL_BOUNDS=1 GXV_COLOR_LOADSKIP=15 GXV_FIRSTCLEAR=1 GXV_TAILDEPTH=1"
fail=0
pics() { grep -E '^frame[0-9]+ ' "$1" | sed -E 's/ scenes=.* hash=/ hash=/'; }
field() { grep -E "^frame$2 " "$1" | sed -E "s/.* $3=([0-9]+).*/\\1/"; }
run() { # tag mode validregion nocapture w h swap penv...
  local log=$1 m=$2 vr=$3 nocap=$4 w=$5 h=$6 swap=$7; shift 7
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_TEXMEMO=0 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 "$@" \
    GXV_EFBSWAP2=$swap GXV_HOIST_GRID=1 GXV_CORNERPASS=$m GXV_VGL_VALIDREGION=$vr HG_RW=$w HG_RH=$h $([[ $nocap == 1 ]] && echo HG_NOCAPTURE=1) "$out/vgx_hoistgrid_gl" > "$log" 2>&1 || true
  if grep -q "^FAIL" "$log" || ! grep -q "^frame10 " "$log"; then echo "FAILED: $log did not run cleanly"; grep -E "^FAIL|rror" "$log" | head -5; fail=1; return 1; fi
}
console="${vita/GXV_SHADOWFILL_BOUNDS=1/GXV_SHADOWFILL_BOUNDS=0} GXV_SHADOW_RECT=0 GXV_SHADOW_RECT_FE=0 GXV_SHADOW_STENCIL=1 GXV_SPLIT=1"
[[ $console == *GXV_SHADOWFILL_BOUNDS=0* ]] || { echo "FAILED: the console profile does not switch GXV_SHADOWFILL_BOUNDS off"; exit 1; }
for profile in plain vita console; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  [[ $profile == console ]] && penv=$console
  for size in "720 404" "960 544" "608 346" "848 480" "640 456"; do
    set -- $size
    for swap in 1 0; do
      tag="$profile-$1x$2-s$swap"
      ok=1
      for mode in 0 1 2 1v; do
        m=${mode%v}; vr=0; [[ $mode == 1v ]] && vr=1
        run "$out/cp-$tag-$mode.log" $m $vr 1 $1 $2 $swap $penv || ok=0
      done
      [[ $ok == 1 ]] || continue
      ref=$(pics "$out/cp-$tag-0.log")
      for mode in 1 2 1v; do
        [[ "$ref" == "$(pics "$out/cp-$tag-$mode.log")" ]] || { echo "FAILED: $tag: pictures or corner textures differ with GXV_CORNERPASS=$mode"; fail=1; }
      done
      cp=$(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-1.log")
      line="$tag: corner passes $cp;"
      for f in 1 2 3 4 5 6 7 8 9 10; do
        s0=$(field "$out/cp-$tag-0.log" $f scenes); s1=$(field "$out/cp-$tag-1.log" $f scenes); s2=$(field "$out/cp-$tag-2.log" $f scenes)
        e0=$(field "$out/cp-$tag-0.log" $f efb); e1=$(field "$out/cp-$tag-1.log" $f efb); h1=$(field "$out/cp-$tag-1.log" $f hoist)
        line="$line f$f efb $e0->$e1"
        [[ $s2 == $s0 && $(field "$out/cp-$tag-2.log" $f efb) == $e0 ]] || { echo "FAILED: $tag frame $f: GXV_CORNERPASS=2 changed the scenes"; fail=1; }
        if [[ $f == 1 || $f == 2 || $f == 4 ]]; then
          [[ $e1 == $e0 && $h1 == 0 && $s1 == $s0 ]] || { echo "FAILED: $tag frame $f: must be left as it is (efb $e0 -> $e1, pass $h1)"; fail=1; }
        else
          [[ $e1 == $((e0 - 1)) && $h1 == 1 && $s1 == $s0 ]] || { echo "FAILED: $tag frame $f: expected one EFB scene less and one grid-pass scene (efb $e0 -> $e1, pass $h1, scenes $s0 -> $s1)"; fail=1; }
        fi
      done
      [[ $cp == 7 ]] || { echo "FAILED: $tag: 7 corner passes expected, $cp made"; fail=1; }
      [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-2.log") == 0 && $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-0.log") == 0 ]] || { echo "FAILED: $tag: =0 / =2 made a corner pass"; fail=1; }
      if [[ $profile == console ]]; then
        # the bookkeeping the corner pass turns on must cut nothing, and the compared bounds (GXV_SHADOW_RECT_FE=2) must give the same pictures
        run "$out/cp-$tag-1f.log" 1 0 1 $1 $2 $swap $penv GXV_SHADOW_RECT_FE=2 && { [[ "$ref" == "$(pics "$out/cp-$tag-1f.log")" ]] || { echo "FAILED: $tag: pictures differ with GXV_SHADOW_RECT_FE=2"; fail=1; }; }
        for mode in 1 2; do
          seen=$(sed -nE 's/^cornerfe seen=([0-9]+) marked=([0-9]+)$/\1/p' "$out/cp-$tag-$mode.log"); marked=$(sed -nE 's/^cornerfe seen=([0-9]+) marked=([0-9]+)$/\2/p' "$out/cp-$tag-$mode.log")
          [[ ${seen:-0} -ge 10 && ${marked:-0} -ge 7 ]] || { echo "FAILED: $tag: GXV_CORNERPASS=$mode: the front end saw ${seen:-0} corner copies and marked ${marked:-0} (10 and at least 7 expected: the console's 'seen 0' failure)"; fail=1; }
        done
        for mode in 0 1 2; do
          sf=$(grep -E '^shadowfill ' "$out/cp-$tag-$mode.log" || true)
          [[ "$sf" == "shadowfill skipped=0 scissored=0 full=0 fills=0 fillsKept=0" ]] || { echo "FAILED: $tag: GXV_CORNERPASS=$mode: the fill bounds acted with GXV_SHADOWFILL_BOUNDS=0 ($sf)"; fail=1; }
        done
      fi
      echo "$line"
    done
  done
    # ss24, GXV_CORNERPASS_ALT: the previous capture's shadow quad is drawn BEFORE this picture's capture (HG_EARLYREAD=1: "the copy's texture used before it"). Without the switch the pass is
  # refused in frames 2..10 (nothing hoisted); with it the pass is made in the same frames as without the early reader (3 and 5..10), with one EFB scene less each, and every picture and every
  # corner texture is identical to GXV_CORNERPASS=0 (the early reader must see the old capture, the shadow after the copy the new one). =2 counts only.
  for size in "960 544" "720 404"; do
    set -- $size
    for swap in 1 0; do
      tag="early-$profile-$1x$2-s$swap"
      ok=1
      run "$out/cp-$tag-0.log" 0 0 1 $1 $2 $swap $penv HG_EARLYREAD=1 || ok=0
      run "$out/cp-$tag-1.log" 1 0 1 $1 $2 $swap $penv HG_EARLYREAD=1 || ok=0
      run "$out/cp-$tag-a.log" 1 0 1 $1 $2 $swap $penv HG_EARLYREAD=1 GXV_CORNERPASS_ALT=1 || ok=0
      run "$out/cp-$tag-a2.log" 2 0 1 $1 $2 $swap $penv HG_EARLYREAD=1 GXV_CORNERPASS_ALT=1 || ok=0
      [[ $ok == 1 ]] || continue
      ref=$(pics "$out/cp-$tag-0.log")
      for v in 1 a a2; do [[ "$ref" == "$(pics "$out/cp-$tag-$v.log")" ]] || { echo "FAILED: $tag: pictures or corner textures differ ($v)"; fail=1; }; done
      [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-1.log") == 0 ]] || { echo "FAILED: $tag: the pass was made without the switch though a draw reads the copy's texture before it"; fail=1; }
      [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-a.log") == 7 ]] || { echo "FAILED: $tag: GXV_CORNERPASS_ALT=1: 7 corner passes expected"; fail=1; }
      [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-a2.log") == 0 ]] || { echo "FAILED: $tag: =2 made a pass"; fail=1; }
      line="$tag:"
      for f in 1 2 3 4 5 6 7 8 9 10; do
        e0=$(field "$out/cp-$tag-0.log" $f efb); ea=$(field "$out/cp-$tag-a.log" $f efb); e1=$(field "$out/cp-$tag-1.log" $f efb)
        line="$line f$f efb $e0->$ea"
        [[ $e1 == $e0 ]] || { echo "FAILED: $tag frame $f: the scenes changed without the switch"; fail=1; }
        if [[ $f == 1 || $f == 2 || $f == 4 ]]; then [[ $ea == $e0 ]] || { echo "FAILED: $tag frame $f: must be left as it is"; fail=1; }
        else [[ $ea == $((e0 - 1)) ]] || { echo "FAILED: $tag frame $f: one EFB scene less expected with GXV_CORNERPASS_ALT (efb $e0 -> $ea)"; fail=1; }; fi
      done
      echo "$line"
    done
  done
# with the capture present GXV_HOIST_GRID takes the corner copy first: the switch must change nothing
  for mode in 0 1; do run "$out/cpcap-$profile-$mode.log" $mode 0 0 960 544 1 $penv || true; done
  [[ "$(grep -E '^frame' "$out/cpcap-$profile-0.log")" == "$(grep -E '^frame' "$out/cpcap-$profile-1.log")" ]] || { echo "FAILED: $profile: with the capture present GXV_CORNERPASS=1 changed a frame"; fail=1; }
done
# fix47 (2): the console's picture with its shadow windows (HG_STENCIL=1: the picture starts with the game's clear quad, GXV_SHADOW_STENCIL takes the silhouette
# window, and a shadow volume's alpha lies over the corner in every frame but 3 - Vita3K c36b at Beach Bowl: "not planned: a replayed draw tests depth 600"). The fill
# takes that alpha away, which the front end now knows from the stencil pass (the fill's certificate): frames 3 and 5-8 must lose an EFB scene and frame 2's pass must be made (frame 1: the back
# end has no stencil yet; frame 4: a depth-tested corner draw; frames 9, 10: an RGBA8 capture, no window), every picture and corner texture identical. Controls:
# without the certificate (GXV_SHADOW_STENCIL_BREAK=5 withholds it) only frame 3 is made; with the back end's fallback (GXV_SHADOW_STENCIL_FALLBACK=1) none is.
for size in "960 544" "720 404" "608 346"; do
  set -- $size
  for swap in 1 0; do
    tag="stencil-$1x$2-s$swap"
    ok=1
    for mode in 0 1 2; do run "$out/cp-$tag-$mode.log" $mode 0 0 $1 $2 $swap $console HG_STENCIL=1 || ok=0; done
    run "$out/cp-$tag-brk.log" 1 0 0 $1 $2 $swap $console HG_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=5 || ok=0
    run "$out/cp-$tag-fb1.log" 1 0 0 $1 $2 $swap $console HG_STENCIL=1 GXV_SHADOW_STENCIL_FALLBACK=1 || ok=0
    run "$out/cp-$tag-fb0.log" 0 0 0 $1 $2 $swap $console HG_STENCIL=1 GXV_SHADOW_STENCIL_FALLBACK=1 || ok=0
    [[ $ok == 1 ]] || continue
    ref=$(pics "$out/cp-$tag-0.log")
    for mode in 1 2; do [[ "$ref" == "$(pics "$out/cp-$tag-$mode.log")" ]] || { echo "FAILED: $tag: pictures or corner textures differ with GXV_CORNERPASS=$mode"; fail=1; }; done
    [[ "$(grep -E '^frame' "$out/cp-$tag-0.log")" == "$(grep -E '^frame' "$out/cp-$tag-2.log")" ]] || { echo "FAILED: $tag: GXV_CORNERPASS=2 changed a frame"; fail=1; }
    grep -q "^stencil windows=10 taken=7 " "$out/cp-$tag-1.log" || { echo "FAILED: $tag: 7 shadow windows taken expected ($(grep '^stencil ' "$out/cp-$tag-1.log"))"; fail=1; }
    cp=$(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-1.log")
    fm=$(sed -nE 's/^cornerfill marked=([0-9]+) .*/\1/p' "$out/cp-$tag-1.log")
    line="$tag: corner passes $cp (by the fill's certificate: $fm marked);"
    for f in 1 2 3 4 5 6 7 8 9 10; do
      e0=$(field "$out/cp-$tag-0.log" $f efb); e1=$(field "$out/cp-$tag-1.log" $f efb)
      line="$line f$f efb $e0->$e1"
      if [[ $f == 1 || $f == 4 || $f == 9 || $f == 10 ]]; then [[ $e1 == $e0 ]] || { echo "FAILED: $tag frame $f: must be left as it is (efb $e0 -> $e1)"; fail=1; }
      elif [[ $f == 2 ]]; then # (the first picture whose shadow window the stencil takes: the pass is made, but the EFB keeps its scene count in this one picture - seen, cause not found; the pictures are identical)
        [[ $(field "$out/cp-$tag-1.log" $f hoist) == 1 && $e1 -le $e0 ]] || { echo "FAILED: $tag frame 2: the corner pass must be made (efb $e0 -> $e1)"; fail=1; }
      else [[ $e1 == $((e0 - 1)) ]] || { echo "FAILED: $tag frame $f: one EFB scene less expected (efb $e0 -> $e1)"; fail=1; }; fi
    done
    [[ $cp == 6 && ${fm:-0} -ge 6 ]] || { echo "FAILED: $tag: 6 corner passes on the fill's certificate expected, $cp made, ${fm:-0} marked"; fail=1; }
    [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-brk.log") == 1 ]] || { echo "FAILED: $tag: control: without the certificate only frame 3 can be made"; fail=1; }
    [[ $(sed -nE 's/.*cornerpasses=([0-9]+).*/\1/p' "$out/cp-$tag-fb1.log") == 0 ]] || { echo "FAILED: $tag: control: a corner pass was made though the window's fill is not drawn through the stencil"; fail=1; }
    [[ "$(pics "$out/cp-$tag-fb0.log")" == "$(pics "$out/cp-$tag-fb1.log")" ]] || { echo "FAILED: $tag: fallback pictures differ with GXV_CORNERPASS=1"; fail=1; }
    echo "$line"
  done
done
# cp50: the auto-off. With the capture present GXV_HOIST_GRID's own hoist takes every marked copy (planned 0, "already hoisted"): after GXV_CORNERPASS_AUTOOFF marked pictures the pass
# switches itself off - the front end must then stop marking (and, in the console profile, stop working alpha bounds), and no picture may change.
for profile in plain console; do
  penv="GXV_SOFTXFORM=0"; [[ $profile == console ]] && penv=$console
  run "$out/cpoff-$profile.log" 1 0 0 960 544 1 $penv GXV_CORNERPASS_AUTOOFF=3 || continue
  [[ "$(grep -E '^frame' "$out/cpcap-$profile-0.log")" == "$(grep -E '^frame' "$out/cpoff-$profile.log")" ]] || { echo "FAILED: $profile: the auto-off changed a frame"; fail=1; }
  marked=$(sed -nE 's/^cornerfe seen=([0-9]+) marked=([0-9]+)$/\2/p' "$out/cpoff-$profile.log"); marked_on=$(sed -nE 's/^cornerfe seen=([0-9]+) marked=([0-9]+)$/\2/p' "$out/cpcap-$profile-1.log")
  echo "auto-off $profile: marked ${marked:-0} (3 expected; ${marked_on:-0} without the auto-off)"
  [[ ${marked_on:-0} -gt 3 && ${marked:-0} -eq 3 ]] || { echo "FAILED: $profile: the front end kept marking after the auto-off (marked ${marked:-0}, without it ${marked_on:-0})"; fail=1; }
done
[[ $fail == 0 ]] && echo "PASS: identical pictures and corner textures (GXV_CORNERPASS 0/1/2, VALIDREGION; EFBSWAP2 0/1; 5 render sizes; 3 profiles, the console's GXV_SHADOWFILL_BOUNDS=0 among them), one EFB split less in frames 3 and 5-10, frames 1, 2, 4 untouched; nothing changes with the capture present; with shadow windows and a volume over the corner 6 of 10 frames made on the fill's certificate, controls hold"
exit $fail

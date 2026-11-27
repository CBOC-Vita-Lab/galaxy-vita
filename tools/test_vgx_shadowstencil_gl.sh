#!/usr/bin/env bash
# GXV_SHADOW_STENCIL (pass24; vgx_draw.cpp ShadowStencilPass, vgx_backend_gl.cpp "shadow stencil"): the production GX front end and GLES back end on Mesa
# (tests/vgx_shadowfill_gl.cpp, SF_STENCIL=1: frames 1-30, the game's frame with SF_TRIPLE=1). With the switch at 1 every picture (all four channels,
# the EFB's alpha included) must be identical to the switch off - overlapping volumes, volumes through the near plane, alpha 0 resets, the
# silhouette, the shadow-rect scissor (GXV_SHADOW_RECT=1), a second window in a picture - at three render sizes, on the depth-texture and the
# renderbuffer surfaces, with the depth plan / first-clear / hand-over switches of the console; the windows that check out are taken and each takes
# the GXM scene count of its picture down (the capture's EFB split and its draw-back); every fall-back reason is met and leaves the picture
# alone; GXV_SHADOW_STENCIL_FALLBACK=1 (the back end cannot use the stencil at the capture) gives the same pictures with today's scene count;
# and each deliberate mistake of GXV_SHADOW_STENCIL_BREAK (wrong operation, no reset, no silhouette, wrong test) changes some picture.
#   tools/test_vgx_shadowstencil_gl.sh [output-dir]   (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /usr/include)
#   SF_CXXFLAGS="-fsanitize=address,undefined -O1 -g" for the sanitizer run (a separate output directory).
set -euo pipefail
root=${SF_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
out=${1:-$(mktemp -d)}
mkdir -p "$out/obj"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
flags=(-std=c++20 -O1 -g0 -w ${SF_CXXFLAGS:-} -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr")
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
  "${CXX:-g++}" ${SF_CXXFLAGS:-} "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfill_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
rm -f "$out"/ss-*.log
fail=0
run() { # tag, then env words
  local tag=$1; shift
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_PASSCHECK=1 SF_TRIPLE=1 SF_STENCIL=1 "$@" \
    "$out/vgx_shadowfill_gl" 2>&1 | grep -v "^libunwind" > "$out/ss-$tag.log" || { echo "FAILED: $tag exited with an error"; tail -3 "$out/ss-$tag.log"; fail=1; }
  grep -E "FAIL|PASSCHECK FAILED" "$out/ss-$tag.log" | sed "s/^/$tag: /" || true
}
pics() { grep -E '^frame[0-9]+ hash' "$1" | sed 's/ scenes.*//' | tr '\n' ' '; }
scn() { grep -E '^frame[0-9]+ hash' "$1" | sed 's/.*scenes=\([0-9]*\).*/\1/' | tr '\n' ' '; }
taken() { grep -E '^frame[0-9]+ hash' "$1" | sed 's/.*taken=\([0-9]*\).*/\1/' | tr '\n' ' '; }
for profile in vita plain; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  for size in "128 128" "160 120" "96 96"; do
    set -- $size
    for variant in "GXV_SHADOW_RECT=1" "GXV_SHADOW_RECT=0 GXV_SHADOWFILL_BOUNDS=1" "GXV_SHADOW_RECT=1 SF_HOOK=1 GXV_DEPTHPLAN=1 GXV_FIRSTCLEAR=1 GXV_EFBSWAP=1 GXV_EFBSWAP2=1" \
                   "GXV_SHADOW_RECT=1 GXV_DEPTH_TEX=0" "GXV_SHADOW_RECT=1 GXV_BE_MERGE=1 GXV_OVERDRAW=1"; do
      vtag=${variant//[ =]/_}
      expect=0; [[ $profile == vita ]] && expect=1
      run "$profile-$1x$2-$vtag-off" $penv SF_RW=$1 SF_RH=$2 $variant GXV_SHADOW_STENCIL=0
      run "$profile-$1x$2-$vtag-on" $penv SF_RW=$1 SF_RH=$2 $variant GXV_SHADOW_STENCIL=1 SF_STENCIL_EXPECT=$expect
      run "$profile-$1x$2-$vtag-plan" $penv SF_RW=$1 SF_RH=$2 $variant GXV_SHADOW_STENCIL=2 SF_STENCIL_EXPECT=$expect
      ref=$(pics "$out/ss-$profile-$1x$2-$vtag-off.log")
      for sw in on plan; do
        h=$(pics "$out/ss-$profile-$1x$2-$vtag-$sw.log")
        [[ "$h" == "$ref" ]] || { echo "FAILED: $profile-$1x$2-$vtag-$sw pictures differ from the switch off"; fail=1; }
      done
      # the scene counts: a taken frame has fewer scenes than with the switch off; no other frame has more
      so=($(scn "$out/ss-$profile-$1x$2-$vtag-off.log")); sn=($(scn "$out/ss-$profile-$1x$2-$vtag-on.log")); tk=($(taken "$out/ss-$profile-$1x$2-$vtag-on.log"))
      nt=0; bad=0
      for i in "${!so[@]}"; do
        if [[ ${tk[$i]} -gt 0 ]]; then nt=$((nt+1)); [[ ${sn[$i]} -lt ${so[$i]} ]] || { echo "FAILED: $profile-$1x$2-$vtag frame $((i+1)) taken but scenes ${sn[$i]} (off ${so[$i]})"; bad=1; }
        else [[ ${sn[$i]} -le ${so[$i]} ]] || { echo "FAILED: $profile-$1x$2-$vtag frame $((i+1)) not taken but scenes ${sn[$i]} > off ${so[$i]}"; bad=1; }; fi
      done
      [[ $bad == 0 ]] || fail=1
      [[ $profile == plain || $variant == *SHADOW_RECT=0* && $nt -ge 1 || $nt -ge 14 ]] || { echo "FAILED: $profile-$1x$2-$vtag only $nt windows taken"; fail=1; }
      [[ $profile == vita ]] && echo "$profile-$1x$2-$vtag: $nt frames taken, scenes off [${so[*]}] on [${sn[*]}]"
    done
  done
done
# the back end cannot use the stencil at the capture (a multisampled EFB, an effects pass...): the copy is made there, the same pictures, today's scenes
for size in "128 128" "160 120"; do
  set -- $size
  run "fb-$1x$2-off" $vita SF_RW=$1 SF_RH=$2 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=0
  run "fb-$1x$2-on" $vita SF_RW=$1 SF_RH=$2 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_FALLBACK=1
  [[ "$(pics "$out/ss-fb-$1x$2-off.log")" == "$(pics "$out/ss-fb-$1x$2-on.log")" ]] || { echo "FAILED: fb-$1x$2 pictures differ"; fail=1; }
  so=($(scn "$out/ss-fb-$1x$2-off.log")); sn=($(scn "$out/ss-fb-$1x$2-on.log"))
  for i in "${!so[@]}"; do [[ ${sn[$i]} -le $((so[$i]+0)) ]] || { echo "FAILED: fb-$1x$2 frame $((i+1)) scenes ${sn[$i]} > off ${so[$i]}"; fail=1; }; done
  echo "fb-$1x$2: forced fall-back: same pictures (scenes off [${so[*]}] fallback [${sn[*]}])"
done
# the render size changes every frame (the quality ladder's rungs 0, 2, 1, 2: the EFB's depth-stencil surface is made again each time)
run "rung-off" $vita SF_RUNGS=1 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=0
run "rung-on" $vita SF_RUNGS=1 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=1 SF_STENCIL_EXPECT=1
[[ "$(pics "$out/ss-rung-off.log")" == "$(pics "$out/ss-rung-on.log")" ]] || { echo "FAILED: rung changes: pictures differ"; fail=1; }
grep -q "taken_frames=4,5,6,7,8,11,13,14,15,16,17,18,19,20,21,24,30," "$out/ss-rung-on.log" || { echo "FAILED: rung changes: the stencil did not survive them (windows not taken)"; fail=1; }
echo "rung changes: same pictures, windows taken at every rung"
# the log line
run "log" $vita SF_RW=128 SF_RH=128 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_LOG_EVERY=10
grep "LOG: shadow-stencil:" "$out/ss-log.log" | head -1 | cut -c1-400 || { echo "FAILED: no shadow-stencil line"; fail=1; }
grep -q "LOG: shadow-stencil:" "$out/ss-log.log" || { echo "FAILED: no shadow-stencil line"; fail=1; }
# controls: each deliberate mistake must change some picture
for brk in 1 2 3 4; do
  run "brk$brk" $vita SF_RW=128 SF_RH=128 GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=$brk
  d=$(diff <(pics "$out/ss-vita-128x128-GXV_SHADOW_RECT_1-off.log" | tr ' ' '\n') <(pics "$out/ss-brk$brk.log" | tr ' ' '\n') | grep -c '^>' || true)
  if [[ $d -eq 0 ]]; then echo "FAILED: control GXV_SHADOW_STENCIL_BREAK=$brk not caught"; fail=1; else echo "brk$brk: control caught ($d frames differ)"; fi
done
grep -l -E "FAIL|PASSCHECK FAILED" "$out"/ss-*.log 2>/dev/null | grep -v "ss-brk" && fail=1 || true
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "PASS: identical pictures with GXV_SHADOW_STENCIL 1 and 2, windows taken with fewer scenes, every fall-back reason met, forced fall-back identical, controls caught"

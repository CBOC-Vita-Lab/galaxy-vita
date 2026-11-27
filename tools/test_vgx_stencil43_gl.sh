#!/usr/bin/env bash
# stencil43: the shadow stencil's third class (a MARK: a constant written over what was there - the player's silhouette as the game draws it, a constant DESTINATION alpha),
# the rule for a volume after a mark, the thresholds, and the silhouette gate's hand-over (GXV_SILH_GATE=1 with GXV_SHADOW_STENCIL=1), through the production GX front end and the
# GLES back end on Mesa (tests/vgx_shadowfill_gl.cpp, SF_STENCIL=3: frames 31-39, 50, 52, 53, the game's frame with SF_TRIPLE=1; SF_NOSIL=1 leaves each silhouette out).
#   31 volumes then the silhouette (the game's order)   32 silhouette only            33 a volume AFTER the mark        34 mark 6 + two SUBTRACT-only volume passes (bound refuses)
#   (34: SUBTRACT-only volume passes after a mark of 6)   35 mark = the fill's threshold 2                    36 mark 1 (below it)          37 volume, mark, alpha 0 reset    38 volumes + a silhouette-shaped writer no class places, hint 1
#   39 the volumes of 38 alone                          50 volumes + the game's silhouette, hint 1                    52 the silhouette alone, hint 1     53 nothing
#   tools/test_vgx_stencil43_gl.sh [output-dir]   (KHR_INCLUDE, default /work/khr/include; SF_SKIP_BUILD=1 to reuse the binary)
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
    o="$out/obj/$unit.o"; newer=0
    for h in "$src/$unit.cpp" "$src"/*.h "$root"/platform/include/port/*.h; do [[ "$h" -nt "$o" ]] && newer=1; done
    if [[ ! -f "$o" || $newer == 1 ]]; then nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$src/$unit.cpp" -o "$o"; fi
    objs+=("$o")
  done
  nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/tests/vgx_shadowfill_gl.cpp" -o "$out/obj/test.o"
  [[ -f "$out/obj/host.o" ]] || nice -n 10 "${CXX:-g++}" "${flags[@]}" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/obj/host.o"
  "${CXX:-g++}" ${SF_CXXFLAGS:-} "${objs[@]}" "$out/obj/test.o" "$out/obj/host.o" -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfill_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
plain="GXV_SOFTXFORM=0"
fail=0
run() { # tag, then env words
  local tag=$1; shift
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_PASSCHECK=1 SF_TRIPLE=1 SF_STENCIL=3 \
      GXV_SHADOW_RECT=1 SF_RW=$RW SF_RH=$RH "$@" "$out/vgx_shadowfill_gl" 2>&1 | grep -v "^libunwind" > "$out/s43-$tag.log" || { echo "FAILED: $tag exited with an error"; tail -3 "$out/s43-$tag.log"; fail=1; }
  grep -E "FAIL|PASSCHECK FAILED" "$out/s43-$tag.log" | sed "s/^/$tag: /" || true
}
hashof() { grep -E "^frame$2 " "$out/s43-$1.log" | sed -E 's/.*hash=([0-9a-f]+).*/\1/'; }
scenesof() { grep -E "^frame$2 " "$out/s43-$1.log" | sed -E 's/.*scenes=([0-9]+).*/\1/'; }
takenof() { grep -E "^frame$2 " "$out/s43-$1.log" | sed -E 's/.*taken=([0-9]+).*/\1/'; }
reasonsof() { grep -E "^frame$2 " "$out/s43-$1.log" | grep -o '\[[^]]*\]' | tr '\n' ' '; }
frames="31 32 33 34 35 36 37 38 39 50 52 53"
expect_taken="31 32 33 35 37 39 50 52"   # (34: volume after a mark, bound; 36: alpha 1 < 2; 38: unplaced writer; 53: nothing)
for size in "128 128" "160 120" "96 96"; do
for profile in vita plain; do
  set -- $size; RW=$1; RH=$2
  [[ $profile == plain && $RW != 128 ]] && continue
  penv=$vita; [[ $profile == plain ]] && penv=$plain
  P=$profile-${RW}x${RH}
  run $P-off $penv GXV_SHADOW_STENCIL=0
  run $P-nosil $penv GXV_SHADOW_STENCIL=0 SF_NOSIL=1
  run $P-s1 $penv GXV_SHADOW_STENCIL=1
  run $P-s2 $penv GXV_SHADOW_STENCIL=2
  run $P-s3 $penv GXV_SHADOW_STENCIL=3 GXV_SHADOW_STENCIL_LOG_EVERY=12
  run $P-g1 $penv GXV_SHADOW_STENCIL=1 GXV_SILH_GATE=1
  run $P-g0 $penv GXV_SHADOW_STENCIL=0 GXV_SILH_GATE=1
  if [[ $profile == plain ]]; then # (no CPU-transformed clear quad: no window begins, nothing is taken; the pictures must still be the same)
    for t in s1 s2 s3 g1 g0; do for f in $frames; do
      [[ "$(hashof $P-off $f)" == "$(hashof $P-$t $f)" || ( $t == g1 || $t == g0 ) ]] || { echo "FAILED: $P-$t frame $f differs"; fail=1; }
    done; done
    continue
  fi
  # 1. the silhouette matters (the picture differs from its twin without it; frame 36: a mark of 1 does not darken, but it overwrites the volume count under it)
  for f in 31 32 33 35 37 38 50 52; do [[ "$(hashof $P-off $f)" != "$(hashof $P-nosil $f)" ]] || { echo "FAILED: frame $f: the silhouette changes nothing (the test would be vacuous)"; fail=1; }; done
  # 2. exact: the stencil path, the plan and the census give the pictures of the capture path
  for t in s1 s2 s3; do for f in $frames; do
    [[ "$(hashof $P-off $f)" == "$(hashof $P-$t $f)" ]] || { echo "FAILED: $P-$t frame $f differs from the capture path"; fail=1; }
  done; done
  # 3. which windows are taken (stencil 1 and the plan 2 agree), and each taken window has fewer GXM scenes
  for f in $frames; do
    want=0; for e in $expect_taken; do [[ $e == $f ]] && want=1; done
    for t in s1 s2 s3; do
      tk=$(takenof $P-$t $f)
      [[ $((tk>0)) == $want ]] || { echo "FAILED: $P-$t frame $f taken=$tk, expected $want"; fail=1; }
    done
    if [[ $want == 1 ]]; then [[ $(scenesof $P-s1 $f) -lt $(scenesof $P-off $f) ]] || { echo "FAILED: frame $f taken but scenes $(scenesof $P-s1 $f) (off $(scenesof $P-off $f))"; fail=1; }
    else [[ $(scenesof $P-s1 $f) -le $(scenesof $P-off $f) ]] || { echo "FAILED: frame $f not taken but more scenes"; fail=1; }; fi
  done
  # 4. the refusals say why
  [[ "$(reasonsof $P-s1 34)" == *"volume after silhouette"* ]] || { echo "FAILED: frame 34 not refused for the bound: $(reasonsof $P-s1 34)"; fail=1; }
  [[ "$(reasonsof $P-s1 36)" == *"alpha value"* ]] || { echo "FAILED: frame 36 not refused for the threshold: $(reasonsof $P-s1 36)"; fail=1; }
  [[ "$(reasonsof $P-s1 38)" == *"unclassified alpha writer"* ]] || { echo "FAILED: frame 38 not refused as unclassified: $(reasonsof $P-s1 38)"; fail=1; }
  # 5. the gate and the stencil together: the gate's own pictures where it acts (the silhouette left out), the stencil's elsewhere; the window of 52 is the gate's whole
  for f in 31 32 33 34 35 36 37 39 53; do [[ "$(hashof $P-off $f)" == "$(hashof $P-g1 $f)" ]] || { echo "FAILED: gate + stencil: frame $f differs from the capture path"; fail=1; }; done
  [[ "$(hashof $P-g1 38)" == "$(hashof $P-off 39)" ]] || { echo "FAILED: gate + stencil: frame 38 is not the picture of its twin 39 (the unplaced silhouette should be left out and the window taken)"; fail=1; }
  [[ "$(hashof $P-g1 50)" == "$(hashof $P-off 39)" ]] || { echo "FAILED: gate + stencil: frame 50 is not the picture of its twin 39 (hint 1: the silhouette's draws are left out)"; fail=1; }
  [[ "$(hashof $P-g1 52)" == "$(hashof $P-off 53)" ]] || { echo "FAILED: gate + stencil: frame 52 is not the picture of 53 (hint 1, no volume: the gate)"; fail=1; }
  [[ $(takenof $P-g1 38) -ge 1 && $(takenof $P-g1 50) -ge 1 ]] || { echo "FAILED: gate + stencil: frames 38 and 50 not taken by the stencil"; fail=1; }
  [[ $(takenof $P-g1 52) -eq 0 ]] || { echo "FAILED: gate + stencil: frame 52 taken by the stencil (the gate has it whole)"; fail=1; }
  [[ $(scenesof $P-g1 38) -lt $(scenesof $P-off 38) && $(scenesof $P-g1 50) -lt $(scenesof $P-off 50) && $(scenesof $P-g1 52) -lt $(scenesof $P-off 52) ]] || { echo "FAILED: gate + stencil: scenes not fewer in frames 38, 50, 52"; fail=1; }
  # the gate alone is as before (a volume keeps the capture: 38 and 50 are whole)
  for f in $frames; do [[ $f == 52 || "$(hashof $P-off $f)" == "$(hashof $P-g0 $f)" ]] || { echo "FAILED: gate alone: frame $f differs"; fail=1; }; done
  # 6. the controls: each wrong rule changes a picture
  run $P-b5 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=5   # (a mark under the threshold is marked)
  run $P-b6 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=6   # (a volume after a mark with no bound)
  run $P-b3 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=3   # (the mark keeps what the counters held)
  run $P-b2 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=2   # (the alpha 0 write does not reset: the wrong order of reset and mark)
  run $P-b1 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=1   # (back faces decrement)
  run $P-b4 $penv GXV_SHADOW_STENCIL=1 GXV_SHADOW_STENCIL_BREAK=4   # (the fill tests "equal")
  [[ "$(hashof $P-off 36)" != "$(hashof $P-b5 36)" ]] && echo "control 5 (wrong threshold): frame 36 differs" || { echo "FAILED: control 5 not caught"; fail=1; }
  [[ "$(hashof $P-off 34)" != "$(hashof $P-b6 34)" ]] && echo "control 6 (no bound for a volume after a mark): frame 34 differs" || { echo "FAILED: control 6 not caught"; fail=1; }
  [[ "$(hashof $P-off 31)" != "$(hashof $P-b3 31)" ]] && echo "control 3 (mark does not set): frame 31 differs" || { echo "FAILED: control 3 not caught"; fail=1; }
  [[ "$(hashof $P-off 37)" != "$(hashof $P-b2 37)" ]] && echo "control 2 (no reset after the mark): frame 37 differs" || { echo "FAILED: control 2 not caught"; fail=1; }
  [[ "$(hashof $P-off 31)" != "$(hashof $P-b1 31)" ]] && echo "control 1 (wrong direction): frame 31 differs" || { echo "FAILED: control 1 not caught"; fail=1; }
  [[ "$(hashof $P-off 31)" != "$(hashof $P-b4 31)" ]] && echo "control 4 (wrong test): frame 31 differs" || { echo "FAILED: control 4 not caught"; fail=1; }
  # 7. the census
  grep -h "LOG: stencil classes" "$out/s43-$P-s3.log" | head -2 | cut -c1-1500
  grep -q "LOG: stencil classes" "$out/s43-$P-s3.log" || { echo "FAILED: no census line"; fail=1; }
  echo "== frame: scenes off / stencil / gate+stencil, taken (stencil)"; for f in $frames; do echo "  frame $f: $(scenesof $P-off $f) / $(scenesof $P-s1 $f) / $(scenesof $P-g1 $f)  taken $(takenof $P-s1 $f) $(reasonsof $P-s1 $f)"; done
done
done
grep -l -E "FAIL|PASSCHECK FAILED" "$out"/s43-*.log 2>/dev/null | grep -v "s43-.*-b[0-9]" && fail=1 || true
[[ $fail -eq 0 ]] || { echo "FAILED"; exit 1; }
echo "ok: stencil43"

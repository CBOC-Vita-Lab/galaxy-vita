#!/usr/bin/env bash
# GXV_FIRSTCLEAR (gpucut23, vgx_backend_gl.cpp PlanFrame): the picture's first EFB scene, which begins with the game's EFB clear quad
# (MainLoopFramework::clearEfb: a GX_CULL_BACK screen quad, z ALWAYS with update), loads no depth. Host proof on Mesa with the production
# front end and GLES back end (tests/vgx_shadowfill_gl.cpp's frames, SF_TRIPLE=1: the game's three-XFB frame), a present hook installed
# as on the console (SF_HOOK=1) and the console's depth memory modelled in every frame (GXV_DEPTHSIM_ALWAYS=1: a scene begun without its
# depth starts with 0, which fails every LEQUAL test):
#   - with the switch on, every frame's picture and alpha capture equal the true picture (no model, switch off), the clear quad drawn
#     (SF_CLEARCULL=1) and culled (=2, the EFB never cleared: nothing may be taken);
#   - the quads taken / loads left out: > 0 with the quad drawn in the console profile, 0 when culled and 0 in the plain profile (no CPU
#     transform: the quad's pixels are not known);
#   - control GXV_FIRSTCLEAR_BREAK=1 (a culled quad taken too) must change the culled case's pictures.
#   tools/test_vgx_firstclear_gl.sh [output-dir]   (Khronos headers: KHR_INCLUDE, default /usr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
# (the shadowfill test's build; its objects are kept in <output-dir>/obj)
SF_ROOT="$root" bash "$root/tools/test_vgx_shadowfill_gl.sh" "$out" > "$out/shadowfill-build.log" 2>&1 || true
[[ -x "$out/vgx_shadowfill_gl" ]] || { echo "FAILED: the shadowfill test did not build"; exit 1; }
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
run() { # <tag> <env...>: the frames' hashes (picture and alpha capture) and the counters
  local tag=$1; shift
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 SF_TRIPLE=1 SF_HOOK=1 \
    GXV_SHADOW_RECT=1 "$@" "$out/vgx_shadowfill_gl" > "$out/fc-$tag.log" 2>&1 || true
  grep -E '^frame[0-9]+ hash' "$out/fc-$tag.log" > "$out/fc-$tag.hash" || true
}
fail=0
for profile in vita plain; do
  penv="GXV_SOFTXFORM=0"
  [[ $profile == vita ]] && penv=$vita
  for size in "128 128" "160 120"; do
    set -- $size
    for cull in 1 2; do
      t="$profile-$1x$2-cull$cull"
      run "$t-truth" $penv SF_RW=$1 SF_RH=$2 SF_CLEARCULL=$cull GXV_FIRSTCLEAR=0
      run "$t-off" $penv SF_RW=$1 SF_RH=$2 SF_CLEARCULL=$cull GXV_FIRSTCLEAR=0 GXV_DEPTHSIM_ALWAYS=1
      run "$t-on" $penv SF_RW=$1 SF_RH=$2 SF_CLEARCULL=$cull GXV_FIRSTCLEAR=1 GXV_DEPTHSIM_ALWAYS=1
      run "$t-brk" $penv SF_RW=$1 SF_RH=$2 SF_CLEARCULL=$cull GXV_FIRSTCLEAR=1 GXV_FIRSTCLEAR_BREAK=1 GXV_DEPTHSIM_ALWAYS=1
      n=$(wc -l < "$out/fc-$t-truth.hash")
      [[ $n -eq 16 ]] || { echo "FAILED: $t: $n frames"; fail=1; }
      on=$(diff "$out/fc-$t-truth.hash" "$out/fc-$t-on.hash" | grep -c '^>' || true)
      off=$(diff "$out/fc-$t-truth.hash" "$out/fc-$t-off.hash" | grep -c '^>' || true)
      brk=$(diff "$out/fc-$t-truth.hash" "$out/fc-$t-brk.hash" | grep -c '^>' || true)
      cnt=$(grep '^firstclear:' "$out/fc-$t-on.log" || echo "firstclear: none")
      echo "$t: switch on vs true picture: $on of 16 frames differ; (switch off under the model: $off differ); control: $brk differ; $cnt"
      [[ $on -eq 0 ]] || { echo "FAILED: $t: GXV_FIRSTCLEAR=1 changes the picture"; fail=1; }
      taken=$(sed -nE 's/.*quads taken ([0-9]+).*/\1/p' "$out/fc-$t-on.log")
      if [[ $profile == vita && $cull == 1 ]]; then
        [[ ${taken:-0} -gt 0 ]] || { echo "FAILED: $t: the clear quad was not taken"; fail=1; }
      else
        [[ ${taken:-0} -eq 0 ]] || { echo "FAILED: $t: a quad was taken where its pixels are not known or it is culled"; fail=1; }
      fi
      if [[ $profile == vita && $cull == 2 ]]; then
        [[ $brk -gt 0 ]] || { echo "FAILED: $t: control GXV_FIRSTCLEAR_BREAK=1 not caught"; fail=1; }
      fi
    done
  done
done
[[ $fail == 0 ]] && echo "ok: GXV_FIRSTCLEAR"
exit $fail

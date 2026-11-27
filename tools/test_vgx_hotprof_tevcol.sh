#!/usr/bin/env bash
# hotprof25 round 2 (11 Nov): GXV_HOTPROF_TEVCOL's front-end side. The production front end on the console-shaped fixture (tests/vgx_fe_work.cpp,
# the null back end), particle workloads. The stream with a particle's TEV colour written as the GX library writes it with the switch on
# (FEW_TEVCOL=1: the BG word once; in an emitter's loop a word equal to the loop's last for its register not at all) must give the same
# command buffers in every frame as the stream of before (FEW_TEVCOL=0), and be shorter. FEW_TEVCOL=2 (verify: everything written, the
# words 1 leaves out announced by markers): the same command buffers, the front end's check counts > 0 words and 0 differing. FEW_TEVCOL=3
# (negative control: a word that is new announced too): differing must be > 0.
#   tools/test_vgx_hotprof_tevcol.sh [outdir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hotprof-tevcol.XXXXXX")}
mkdir -p "$out"
src="$root/platform/vgx/src"
args=()
for unit in state draw fesplit dlcache guestmem bpsync shadergen texcache texdecode stats backend_null texcomp; do args+=("$src/vgx_$unit.cpp"); done
if [[ "${HOTPROF_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++17 -O2 -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -pthread "$root/tests/vgx_fe_work.cpp" "${args[@]}" -o "$out/few" 2> "$out/build.log" \
    || { grep -E "error" "$out/build.log" | head; echo "build FAILED"; exit 1; }
fi
# (the console's front-end settings, as tools/test_vgx_twocore.sh / test_vgx_cache24.sh; FEW_RETAINED set: the scene does not depend on the stream's length)
export GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1 GXV_STATICCULL_PAL=1 GXV_DYNCULL=1 GXV_DLWELD=1 \
       GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24 GXV_FE_PARALLEL=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_ABSIDX=0 GXV_SOFTXFORM=32
export FEW_TRASH=4096 FEW_FUR=10
fails=0
n=0
for wl in "FEW_JPA=300 FEW_FX=60 FEW_FURN=6 FEW_PLANTS=60 FEW_ODD=0" "FEW_JPA=400 FEW_JPAVAR=1 FEW_FX=0 FEW_ODD=0" "FEW_JPA=200 FEW_JPAVAR=1 FEW_FX=30 FEW_ODD=1"; do
  for sp in 0 1 2; do
    for seed in 1 2; do
      ret=$((seed - 1))
      ref=""
      for m in 0 1 2 3; do
        # shellcheck disable=SC2086
        o=$(env $wl FEW_RETAINED=$ret GXV_SPLIT=$sp FEW_TEVCOL=$m FEW_LOG=1 "$out/few" 40 $seed 2>&1)
        h=$(echo "$o" | grep '^frame' | md5sum | cut -c1-16)
        sz=$(echo "$o" | grep -o '[0-9]* stream bytes' | tail -1 | cut -d' ' -f1)
        ver=$(echo "$o" | grep -o 'tevcol verify: [0-9]* checked, [0-9]* differing' | tail -1 || true)
        chk=$(echo "$ver" | sed -n 's/.*: \([0-9]*\) checked.*/\1/p'); dif=$(echo "$ver" | sed -n 's/.*, \([0-9]*\) differing/\1/p')
        n=$((n + 1))
        case $m in
          0) ref=$h; sz0=$sz ;;
          1) [[ "$h" == "$ref" ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: FEW_TEVCOL=1 gives other command buffers"; fails=$((fails + 1)); }
             [[ "$sz" -lt "$sz0" ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: the stream is not shorter ($sz0 -> $sz)"; fails=$((fails + 1)); }
             sz1=$sz ;;
          2) [[ "$h" == "$ref" ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: FEW_TEVCOL=2 gives other command buffers"; fails=$((fails + 1)); }
             [[ -n "$chk" && "$chk" -gt 0 && "$dif" == 0 ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: verify said '$ver'"; fails=$((fails + 1)); }
             ver2=$ver ;;
          3) [[ -n "$dif" && "$dif" -gt 0 ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: the negative control was not seen ('$ver')"; fails=$((fails + 1)); } ;;
        esac
      done
      [[ $seed == 1 && $sp == 1 ]] && echo "[$wl]: stream $sz0 -> $sz1 bytes over 40 frames; $ver2; negative control: $ver"
    done
  done
done
[[ $fails == 0 ]] && echo "test_vgx_hotprof_tevcol: PASS ($n runs)" || { echo "test_vgx_hotprof_tevcol: FAIL ($fails)"; exit 1; }

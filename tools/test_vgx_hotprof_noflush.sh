#!/usr/bin/env bash
# hotprof25 (11 Nov): GXV_HOTPROF_NOFLUSHSTRIP's front-end side. The production front end on the console-shaped fixture (tests/vgx_fe_work.cpp, the null
# back end), particle workloads whose "clip on" emitters send the GX library's all-zero flush strip in front of every list call: the command
# buffers of every frame must be the same when the strips are left out of the stream (FEW_NOFLUSH=1), with GXV_SPLIT 0 / 1 / 2, with and without
# GXV_FXMERGE (there the strip is a count on the call item); without the gathering the stream must be shorter (the strips were there).
#   tools/test_vgx_hotprof_noflush.sh [outdir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/hotprof-noflush.XXXXXX")}
mkdir -p "$out"
src="$root/platform/vgx/src"
args=()
for unit in state draw fesplit dlcache guestmem bpsync shadergen texcache texdecode stats backend_null texcomp; do args+=("$src/vgx_$unit.cpp"); done
if [[ "${HOTPROF_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++17 -O2 -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -pthread "$root/tests/vgx_fe_work.cpp" "${args[@]}" -o "$out/few" 2> "$out/build.log" \
    || { grep -E "error" "$out/build.log" | head; echo "build FAILED"; exit 1; }
fi
# (the console's front-end settings, as tools/test_vgx_twocore.sh / test_vgx_cache24.sh)
export GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1 GXV_STATICCULL_PAL=1 GXV_DYNCULL=1 GXV_DLWELD=1 \
       GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24 GXV_FE_PARALLEL=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_ABSIDX=0 GXV_SOFTXFORM=32
# (FEW_RETAINED set, 0 or 1: the fixture then cuts the stream into pieces from a random sequence of their own, so that a stream of another
# length is the same scene; without it the scene's random values follow the stream's length and the two runs would draw different scenes)
export FEW_TRASH=4096 FEW_FUR=10
fails=0
n=0
for wl in "FEW_JPA=300 FEW_FX=60 FEW_FURN=6 FEW_PLANTS=60 FEW_ODD=0" "FEW_JPA=400 FEW_JPAVAR=1 FEW_FX=0 FEW_ODD=0" "FEW_JPA=150 FEW_FXMERGE=1 FEW_FX=60" "FEW_JPA=300 FEW_JPAVAR=1 FEW_FXMERGE=1 FEW_ODD=1"; do
  for sp in 0 1 2; do
    for seed in 1 2; do
      ret=$((seed - 1)) # (seed 1 without the shapes' handles, seed 2 with them)
      # shellcheck disable=SC2086
      a=$(env $wl FEW_RETAINED=$ret GXV_SPLIT=$sp FEW_LOG=1 "$out/few" 40 $seed 2>&1)
      # shellcheck disable=SC2086
      b=$(env $wl FEW_RETAINED=$ret GXV_SPLIT=$sp FEW_NOFLUSH=1 FEW_LOG=1 "$out/few" 40 $seed 2>&1)
      ha=$(echo "$a" | grep '^frame' | md5sum | cut -c1-16)
      hb=$(echo "$b" | grep '^frame' | md5sum | cut -c1-16)
      fa=$(echo "$a" | grep -c '^frame' || true)
      sa=$(echo "$a" | grep -o '[0-9]* stream bytes' | tail -1 | cut -d' ' -f1)
      sb=$(echo "$b" | grep -o '[0-9]* stream bytes' | tail -1 | cut -d' ' -f1)
      n=$((n + 1))
      [[ "$fa" == 40 ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: $fa frames"; fails=$((fails + 1)); }
      [[ "$ha" == "$hb" ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: the command buffers differ without the strips"; fails=$((fails + 1)); }
      if [[ "$wl" != *FXMERGE* ]]; then
        [[ "$sb" -lt "$sa" ]] || { echo "FAIL: [$wl] GXV_SPLIT=$sp seed $seed: the stream is not shorter without the strips ($sa -> $sb): none were sent"; fails=$((fails + 1)); }
      fi
      [[ $seed == 1 && $sp == 1 ]] && echo "[$wl]: stream $sa -> $sb bytes over 40 frames, command buffers $ha / $hb"
    done
  done
done
[[ $fails == 0 ]] && echo "test_vgx_hotprof_noflush: PASS ($n pairs of runs)" || { echo "test_vgx_hotprof_noflush: FAIL ($fails)"; exit 1; }

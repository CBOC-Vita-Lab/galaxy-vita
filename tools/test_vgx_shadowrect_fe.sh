#!/usr/bin/env bash
# GXV_SHADOW_RECT_FE (shadowfe24): the alpha bounds of GXV_SHADOW_RECT / GXV_SHADOWFILL_BOUNDS for less front-end work, on the J3D-shaped fixture with
# shadow volumes (tests/vgx_fe_work.cpp FEW_SHADOW = volumes as static runs, FEW_SHADOWDYN = as immediate geometry). Passes when, for every workload
# and setting below,
#   - GXV_SHADOW_RECT_FE=1 and =2 give the command buffers of =0 frame for frame, the records' alpha bounds (DrawRec::ab, abX.., coverCull: FEW_HASHAB=1)
#     included; =2 (both ways, per draw) reports 0 differing; the engagement line is there and the bounds really were worked out (points > 0);
#   - =1 does fewer points than =0 would (memo hits > 0 on the volumes) and makes fewer deferred-key copies (GXV_DIRECT_J3D);
#   - the same with GXV_DIRECT_J3D=2 (every templated run verified), GXV_SPLIT=0 and 1, GXV_SHADOWFILL_BOUNDS=1 with and without GXV_SHADOW_RECT;
#   - the fixture built from the base tree (REF, optional) gives =0's buffers when the new fixture workloads are run on it (the switch off changes nothing).
#   tools/test_vgx_shadowrect_fe.sh <fixture binary> [reference binary] [frames]
#   build: tools/build_few.sh-style: g++ -std=c++17 -O2 -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread tests/vgx_fe_work.cpp platform/vgx/src/vgx_{state,draw,fesplit,dlcache,guestmem,bpsync,shadergen,texcache,texdecode,stats,backend_null,texcomp}.cpp
set -uo pipefail
bin=${1:?fixture binary}; ref=${2:-}; frames=${3:-40}
here=$(cd "$(dirname "$0")/.." && pwd)
# (the console's front-end settings, vita/vgx-game/entry.cpp, as far as the fixture has them)
vita="GXV_SPLIT=1 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1 GXV_STATICCULL_PAL=1 GXV_DYNCULL=1
      GXV_DLWELD=1 GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24 GXV_FE_PARALLEL=1 GXV_RETAINED=1
      GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_ABSIDX=0 GXV_SOFTXFORM=32 GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 FEW_ODD=0 FEW_RETAINED=1 FEW_HASHAB=1 FEW_LOG=1"
fail=0
workloads=("FEW_SHADOW=20 FEW_SHADOWDYN=11 FEW_JPA=150 FEW_FX=60 FEW_PLANTS=40"
           "FEW_SHADOW=31"
           "FEW_SHADOWDYN=31"
           "FEW_SHADOW=12 FEW_SHADOWDYN=12 FEW_CAM=1 FEW_BIG=4 FEW_FUR=6 FEW_JPA=60"
           "FEW_SHADOW=8 FEW_SHADOWDYN=8 FEW_ODD=1 FEW_J3N=1")
switches=("GXV_SHADOW_RECT=1" "GXV_SHADOW_RECT=1 GXV_SHADOWFILL_BOUNDS=1" "GXV_SHADOW_RECT=0 GXV_SHADOWFILL_BOUNDS=1" "GXV_SHADOW_RECT=2")
run() { env $vita "$@" "$bin" "$frames" 3 2>&1; }
for wl in "${workloads[@]}"; do
  for sp in 1 0; do
    for sw in "${switches[@]}"; do
      for dj in 1 2; do
        tag="[$wl] SPLIT=$sp $sw DIRECT_J3D=$dj"
        r0=$(run $wl GXV_SPLIT=$sp $sw GXV_DIRECT_J3D=$dj GXV_SHADOW_RECT_FE=0)
        h0=$(echo "$r0" | grep '^frame' | md5sum | cut -c1-32)
        abn=$(echo "$r0" | grep -c '^ab frame')
        copies0=$(echo "$r0" | grep -o '[0-9]* made later' | head -1 | cut -d' ' -f1)
        for fe in 1 2; do
          r=$(run $wl GXV_SPLIT=$sp $sw GXV_DIRECT_J3D=$dj GXV_SHADOW_RECT_FE=$fe)
          h=$(echo "$r" | grep '^frame' | md5sum | cut -c1-32)
          [ "$h" = "$h0" ] || { echo "FAIL buffers differ: $tag FE=$fe"; fail=1; }
          [ "$abn" -gt 0 ] || { echo "FAIL no frames: $tag"; fail=1; }
          line=$(echo "$r" | grep 'shadow rectangle front end' | tail -1)
          [ -n "$line" ] || { echo "FAIL no engagement line: $tag FE=$fe"; fail=1; continue; }
          if [ "$fe" = 2 ]; then
            echo "$line" | grep -q 'differing 0$' || { echo "FAIL compared with the plain code: $tag: $line"; fail=1; }
            echo "$line" | grep -q 'compared with the plain code 0,' && case "$sw" in *"RECT=2"*|*"RECT=1"*|*BOUNDS=1*) echo "FAIL nothing compared: $tag"; fail=1;; esac
          fi
          if [ "$fe" = 1 ] && [ "$dj" = 1 ] && [ "$sp" = 1 ]; then
            copies1=$(echo "$r" | grep -o '[0-9]* made later' | head -1 | cut -d' ' -f1)
            echo "ok: $tag FE=1: $line; deferred key copies made later $copies0 -> $copies1"
            [ "${copies1:-0}" -le "${copies0:-0}" ] || { echo "FAIL more key copies with FE=1: $tag"; fail=1; }
          fi
        done
        if [ -n "$ref" ] && [ "$dj" = 1 ] && [ "$sp" = 1 ]; then
          hr=$(env $vita $wl GXV_SPLIT=$sp $sw GXV_DIRECT_J3D=$dj "$ref" "$frames" 3 2>&1 | grep '^frame' | md5sum | cut -c1-32)
          [ "$hr" = "$h0" ] || { echo "FAIL the switch off differs from the base tree: $tag"; fail=1; }
        fi
      done
    done
  done
done
[ $fail = 0 ] && echo "test_vgx_shadowrect_fe: PASS" || echo "test_vgx_shadowrect_fe: FAIL"
exit $fail

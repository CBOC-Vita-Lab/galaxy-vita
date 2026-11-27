#!/usr/bin/env bash
# cache24 host test: GXV_CACHE24 = 0 / 1 / 3 (and GXV_CACHE24_DIST = 1 / 2 / 3) give identical per-frame command-buffer hashes on the J3D-shaped fixture
# (several workloads, GXV_SPLIT 0 / 1 / 2, retained handles right and wrong, GXV_RETAINED=2 verify, GXV_DIRECT_J3D=1 / 4, draw replay on and off,
# the hints of the older switches on); the verify runs find no mismatch; the "cache24:" engagement line shows the hints with the switch on and
# nothing with it off. The hints (PLD) change no value, so nothing else may change.
#   build:  tools/build_few.sh $PWD out/few24 [-fsanitize=address,undefined | -fsanitize=thread]   (with vgx_texcomp.cpp in its list)
#   usage:  tools/test_vgx_cache24.sh [binary] [frames]
set -o pipefail
here=$(cd "$(dirname "$0")/.." && pwd); bin=${1:-$here/out/few24}; frames=${2:-40}
# the console's front-end settings (vita/vgx-game/entry.cpp defaults; the same as the fixture's usual env)
export GXV_SPLIT=1 GXV_SHADOWFILL_BOUNDS=0 GXV_FECUT_KEYID=1 GXV_FECUT_SLHOT=1 GXV_FECUT_HINT=1 GXV_EDGE_AA=1 GXV_CHARAA_AUTO=1 GXV_STATICCULL=1 GXV_STATICCULL_PAL=1 GXV_DYNCULL=1 GXV_DLWELD=1 GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_TEVIN_CANON=1 GXV_FS_FINCLAMP=1 GXV_LIGHT_DEDUP=2 GXV_DL_VARIANTS=24 GXV_FE_PARALLEL=1 GXV_RETAINED=1 GXV_DIRECT_J3D=1 GXV_RIGIDMERGE=1 GXV_ABSIDX=0 GXV_SOFTXFORM=32
export FEW_ODD=0 FEW_RETAINED=1 FEW_JPA=150 FEW_FX=60 FEW_FUR=10 FEW_PLANTS=40 FEW_TRASH=4096
fail=0
for wl in "FEW_JPA=300 FEW_FX=60 FEW_FURN=6 FEW_PLANTS=60 FEW_ODD=0" "FEW_JPA=0 FEW_FX=0 FEW_ODD=1 FEW_BIG=4" "FEW_JPA=150 FEW_FXMERGE=1 FEW_FX=60" \
          "FEW_RETAINED=2 FEW_ODD=1" "FEW_RETAINED=0" "GXV_DRAWREPLAY=0" "GXV_DRAWREPLAY=2 GXV_FE_PREFETCH=3" "GXV_RETAINED=2" "GXV_DIRECT_J3D=4" "GXV_DIRECT_J3D=0 GXV_RETAINED=0"; do
  for sp in 0 1 2; do
    ref=""
    for cfg in "GXV_CACHE24=0" "GXV_CACHE24=1" "GXV_CACHE24=3" "GXV_CACHE24=1 GXV_CACHE24_DIST=2" "GXV_CACHE24=1 GXV_CACHE24_DIST=3"; do
      out=$(env $wl GXV_SPLIT=$sp $cfg GXV_CACHE24_LOG_MS=0 FEW_LOG=1 "$bin" $frames 1 2>&1)
      h=$(echo "$out" | grep '^frame' | md5sum | cut -c1-32)
      [ -z "$ref" ] && ref=$h
      [ "$h" = "$ref" ] || { echo "FAIL hash: [$wl] SPLIT=$sp $cfg"; fail=1; }
      grep -q -E "mismatching [1-9]|MISMATCH" <<<"$out" && { echo "FAIL verify: [$wl] SPLIT=$sp $cfg"; echo "$out" | grep -E "mismatching [1-9]|MISMATCH" | head -2; fail=1; }
      case "$cfg" in
        GXV_CACHE24=0) grep -q '^cache24:' <<<"$out" && { echo "FAIL: cache24 line with the switch off: [$wl] SPLIT=$sp"; fail=1; } ;;
        *) hints=$(echo "$out" | grep '^cache24:' | tail -1 | sed -n 's/.*front-end hints \([0-9.]*\),.*/\1/p')
           [ -n "$hints" ] && [ "${hints%.*}" -gt 0 ] || { echo "FAIL no engagement (front-end hints '${hints:-none}'): [$wl] SPLIT=$sp $cfg"; fail=1; } ;;
      esac
    done
  done
done
[ $fail = 0 ] && echo "test_vgx_cache24: PASS"; exit $fail

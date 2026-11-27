#!/usr/bin/env bash
# GXV_VTXPACK (cached display lists' static runs keep the GX source's compact fixed-point attributes, the 2^-k of each folded into the
# matrix blocks that multiply it; vgx_dlcache.cpp "packed static runs", vgx_draw.cpp VqScaleBlocks): the production GX front end and the
# production GLES back end on Mesa (tests/vgx_vtxpack_gl.cpp: shapes in s16 / s8 / u16 / u8 / float formats, rigid and with two or three
# joints, lit, vertex colours, texgens from tex0 / tex1 / the position / the normal, projective, a normalised post matrix, lists with a
# triangle and a line run, a texture-matrix index per vertex, small lists in the arena). Every frame's picture must be bit-identical with
# the switch off (0) and on (1), under the other front-end switches (GXV_ABSIDX 0/1 - the Vita build uses 0 -, GXV_DLWELD 0/1, GXV_SPLIT
# 0/1/2, GXV_SOFTXFORM 128/0, GXV_UNIPACK 0/1, GXV_DLVCACHE, GXV_DRAWREPLAY, GXV_RIGIDMERGE, GXV_STATICCULL, GXV_VSOUT / GXV_TEXVEC2,
# GXV_VS_TRIM); with it on, runs must be packed, records drawn from packed runs, no matrix element too small to scale and no key refused;
# with it off, nothing packed. The "vtxfeed:" probe line of one run gives the bytes per vertex fed before and after.
#   tools/test_vgx_vtxpack_gl.sh [output-dir]       (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${GXV_VTXPACK_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_vtxpack_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_vtxpack_gl"
fi
frames=${VTXPACK_FRAMES:-24}
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_VTXPACK -u GXV_ABSIDX -u GXV_DLWELD -u GXV_SPLIT -u GXV_SOFTXFORM -u GXV_UNIPACK -u GXV_DLVCACHE -u GXV_DRAWREPLAY -u GXV_RIGIDMERGE \
    -u GXV_STATICCULL -u GXV_VSOUT -u GXV_TEXVEC2 -u GXV_VS_TRIM -u GXV_VTXFEED -u GXV_VTXFEED_MS -u GXV_FE_PARALLEL \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 \
    "$@" "$out/vgx_vtxpack_gl" "$frames" 2>&1 | grep -v '^libunwind' > "$out/$tag.log" || true
  grep -q -E "^FAIL|MISMATCH" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "^FAIL|MISMATCH" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^picture' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep -E '^picture' "$out/$tag.log" | sed 's/.*hash //' > "$out/$tag.hash"
  grep -E '^packedrecords ' "$out/$tag.log" | sed 's/packedrecords //' > "$out/$tag.packed"
  grep -E '^packedruns ' "$out/$tag.log" | sed 's/packedruns //' > "$out/$tag.runs"
}
pair() { # <tag> env assignments...: the switch off and on, the same picture; on packs, off does not
  local tag=$1; shift
  run "$tag-off" "$@" GXV_VTXPACK=0
  run "$tag-on" "$@" GXV_VTXPACK=1
  [[ "$(cat "$out/$tag-off.hash")" == "$(cat "$out/$tag-on.hash")" ]] || { echo "FAIL: $tag: off $(cat "$out/$tag-off.hash"), on $(cat "$out/$tag-on.hash")"; exit 1; }
  [[ "$(cat "$out/$tag-off.packed")" == 0 && "$(cat "$out/$tag-off.runs")" == 0 ]] || { echo "FAIL: $tag: packed with the switch off"; exit 1; }
  [[ "$(cat "$out/$tag-on.packed")" -gt 0 && "$(cat "$out/$tag-on.runs")" -gt 0 ]] || { echo "FAIL: $tag: nothing packed with the switch on"; exit 1; }
  n=$((n + 2))
  printf '%-40s %s  packed runs %3s, packed records %4s\n' "$tag" "$(cat "$out/$tag-on.hash")" "$(cat "$out/$tag-on.runs")" "$(cat "$out/$tag-on.packed")"
}
n=0
for abs in 0 1; do
  for weld in 0 1; do
    for split in 0 1 2; do
      for soft in 128 0; do
        pair "abs$abs-weld$weld-split$split-soft$soft" GXV_ABSIDX=$abs GXV_DLWELD=$weld GXV_SPLIT=$split GXV_SOFTXFORM=$soft
      done
    done
  done
done
for abs in 0 1; do
  pair "abs$abs-unipack0" GXV_ABSIDX=$abs GXV_UNIPACK=0
  pair "abs$abs-vcache" GXV_ABSIDX=$abs GXV_DLWELD=1 GXV_DLVCACHE=1
  pair "abs$abs-replay2" GXV_ABSIDX=$abs GXV_DRAWREPLAY=2
  pair "abs$abs-replay0-rigid0-cull0" GXV_ABSIDX=$abs GXV_DRAWREPLAY=0 GXV_RIGIDMERGE=0 GXV_STATICCULL=0
  pair "abs$abs-rigid1-split1" GXV_ABSIDX=$abs GXV_RIGIDMERGE=1 GXV_SPLIT=1
  pair "abs$abs-vsout-texvec2-vstrim" GXV_ABSIDX=$abs GXV_VSOUT=1 GXV_TEXVEC2=1 GXV_VS_TRIM=1
  pair "abs$abs-feparallel-split1" GXV_ABSIDX=$abs GXV_FE_PARALLEL=1 GXV_SPLIT=1
done
# the PS Vita's defaults (every GXV_ switch vita/vgx-game/entry.cpp sets, relative indices) around the switch: GXV_DIRECT_J3D, GXV_RETAINED,
# GXV_FE_PARALLEL, GXV_DLWELD, GXV_VSOUT, GXV_TEXVEC2, GXV_LIGHT_DEDUP, GXV_STATICCULL_PAL, GXV_DYNCULL ... as the console runs them
vita=$(python3 - "$root/vita/vgx-game/entry.cpp" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
env = {}
for m in re.finditer(r'setenv\("([A-Z0-9_]+)",\s*"([^"]*)"', s): env.setdefault(m.group(1), m.group(2))
for m in re.finditer(r'for \(const char\* name : \{([^}]*)\}\) setenv\(name, "([^"]*)"', s):
    for n in re.findall(r'"([A-Z0-9_]+)"', m.group(1)): env.setdefault(n, m.group(2))
print(' '.join('%s=%s' % (k, v) for k, v in env.items() if k.startswith('GXV_') and k != 'GXV_VTXPACK'))
PY
)
# (the switches of the picture the host draws and reads back stay as the runs above have them)
pair "vita-defaults" $vita GXV_ABSIDX=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0
# every run draws the same pictures (all these switches are exact)
ref=$(cat "$out/abs1-weld0-split0-soft128-off.hash")
for f in "$out"/*.hash; do
  [[ "$(cat "$f")" == "$ref" ]] || { echo "FAIL: $(basename "$f" .hash) draws $(cat "$f"), not $ref"; exit 1; }
done
echo "pictures identical across $n runs: $ref"
# the bytes fed (the probe line, one per picture with GXV_VTXFEED_MS=0: the last one)
for v in 0 1; do
  run "feed$v" GXV_ABSIDX=0 GXV_VTXPACK=$v GXV_VTXFEED=1 GXV_VTXFEED_MS=0
  grep -E '^vtxfeed' "$out/feed$v.log" | tail -1
done
grep -E '^packing' "$out/feed1.log"
# GXV_DIRECT_J3D (on by default since 2.3) with packed runs: the J3D-shaped scene of tests/vgx_fe_work.cpp (retained shape handles, the
# templated resolution that defers the copies of the shader keys) in s16 formats (FEW_S16=1), null back end: the command buffers of every
# frame must be the same with GXV_DIRECT_J3D=0 / 1 / 2 under the PS Vita's defaults. (2.3 c12: the packed runs read a stale key there.)
if [[ "${GXV_VTXPACK_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++17 -g -O2 -I"$src" -I"$root/platform/vgx/third_party" -pthread "$root/tests/vgx_fe_work.cpp" \
    "$src/vgx_state.cpp" "$src/vgx_draw.cpp" "$src/vgx_fesplit.cpp" "$src/vgx_dlcache.cpp" "$src/vgx_guestmem.cpp" "$src/vgx_bpsync.cpp" \
    "$src/vgx_shadergen.cpp" "$src/vgx_texcache.cpp" "$src/vgx_texdecode.cpp" "$src/vgx_stats.cpp" "$src/vgx_backend_null.cpp" -o "$out/few"
fi
for split in 0 1 2; do
  for odd in 0 1; do
    ref=""
    for dj in 0 1 2; do
      h=$(env $vita GXV_ABSIDX=0 GXV_SPLIT=$split GXV_VTXPACK=1 GXV_DIRECT_J3D=$dj FEW_S16=1 FEW_RETAINED=1 FEW_ODD=$odd "$out/few" 60 1 2> "$out/few.err" | md5sum | cut -d' ' -f1)
      grep -q "direct J3D.*verify: [0-9]* checked, [1-9][0-9]* mismatching" "$out/few.err" && { echo "FAIL: direct J3D verify mismatches (split $split odd $odd)"; exit 1; }
      [[ -z "$ref" ]] && ref=$h
      [[ "$h" == "$ref" ]] || { echo "FAIL: GXV_VTXPACK=1 GXV_SPLIT=$split FEW_ODD=$odd: GXV_DIRECT_J3D=$dj makes other command buffers than 0"; exit 1; }
    done
    off=$(env $vita GXV_ABSIDX=0 GXV_SPLIT=$split GXV_VTXPACK=0 GXV_DIRECT_J3D=1 FEW_S16=1 FEW_RETAINED=1 FEW_ODD=$odd "$out/few" 60 1 2>/dev/null | md5sum | cut -d' ' -f1)
    [[ "$off" != "$ref" ]] || { echo "FAIL: the J3D scene packed nothing (split $split odd $odd)"; exit 1; }
    echo "J3D scene, PS Vita defaults, GXV_SPLIT=$split FEW_ODD=$odd: GXV_DIRECT_J3D=0/1/2 the same command buffers with GXV_VTXPACK=1"
  done
done
echo "PASS"

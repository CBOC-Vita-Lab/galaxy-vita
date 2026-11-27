#!/usr/bin/env bash
# GXV_BLOOM_FAST (bloom23): the bloom's glow chain through the PRODUCTION code: decomp BloomEffect.cpp + ImageEffectLocalUtil.cpp (this
# tree's, and BF_BASE's for the reference), the console's GX library (decomp RVL_SDK/gx, TARGET_PC), the vgx front end (ProcessStream of the
# GX FIFO, as vgx_galaxy.cpp) and the GLES back end on Mesa (tests/bloom_fast_gl_{game,main,gx}.*). Each run: BF_FRAMES pictures of a 3D
# scene with glow sources (sun at the screen edge, a lamp behind a pillar, star bits, a thread, a dim surface and a ramp at the threshold).
#   tools/test_bloom_fast_gl.sh [out-dir]       env: BF_BASE=<base tree> (default: none = no base comparison), KHR_INCLUDE (/work/khr/include)
# Checks: GXV_BLOOM_FAST=0 gives the base tree's bloom GX bytes and pictures exactly; 1 gives fewer scenes, copies and draws, and a picture
# within the PSNR floor (tools/bloom_fast_cmp.py); without glow sources (PETARI_BLOOM_SKIP's case) every mode equals the base.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
khr=${KHR_INCLUDE:-/work/khr/include}
src="$root/platform/vgx/src"
W="-w -Wno-register"
CW="-w -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types"
GINC=(-DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 -I"$root/vita/include" -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
      -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions -ffunction-sections
      -fdata-sections -include "$root/vita/include/vita_source_prelude.h" -include "$root/platform/include/port/compat.h" -fno-pie)
CC=${CC:-clang}; CXX=${CXX:-clang++}
build() { # build <tree> <objdir> <exe>
  local tree=$1 o=$2 exe=$3
  mkdir -p "$o"
  local objs=()
  for u in GXAttr GXBump GXDisplayList GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPixel GXTev GXTexture GXTransform; do
    [[ -f "$out/gx/$u.o" ]] || { mkdir -p "$out/gx"; nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/decomp/src/RVL_SDK/gx/$u.c" -o "$out/gx/$u.o"; }
    objs+=("$out/gx/$u.o")
  done
  [[ -f "$out/gx/mtx.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/platform/src/sdk/mtx.c" -o "$out/gx/mtx.o"
  objs+=("$out/gx/mtx.o")
  [[ -f "$out/gx/stub.o" ]] || nice "$CC" -std=gnu11 -O1 -c "${GINC[@]}" $CW "$root/tests/bloom_fast_gl_gx.c" -o "$out/gx/stub.o"
  [[ -f "$out/gx/jma.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/JSystem/JMath/JMATrigonometric.cpp" -o "$out/gx/jma.o"
  [[ -f "$out/gx/cfn.o" ]] || nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/decomp/src/Game/Screen/CopyFilterNegater.cpp" -o "$out/gx/cfn.o"
  objs+=("$out/gx/stub.o" "$out/gx/jma.o" "$out/gx/cfn.o")
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W -I"$root/decomp/include" "$tree/decomp/src/Game/Screen/BloomEffect.cpp" -o "$o/bloom.o"
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$tree/decomp/src/Game/Screen/ImageEffectLocalUtil.cpp" -o "$o/ieu.o"
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$tree/decomp/src/Game/Screen/BloomEffectSimple.cpp" -o "$o/simple.o"
  nice "$CXX" -std=gnu++20 -O1 -c "${GINC[@]}" $W "$root/tests/bloom_fast_gl_game.cpp" -o "$o/game.o"
  mkdir -p "$out/vgx"
  for u in vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_stats vgx_backend_gl vgx_autoq vgx_texcomp; do
    [[ -f "$src/$u.cpp" ]] || continue  # (vgx_texcomp: from texcomp23 on)
    [[ -f "$out/vgx/$u.o" ]] || nice g++ -std=c++20 -O1 -g0 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$src/$u.cpp" -o "$out/vgx/$u.o"
    objs+=("$out/vgx/$u.o")
  done
  [[ -f "$out/vgx/host.o" ]] || nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/platform/vgx/host/vgx_host_egl.cpp" -o "$out/vgx/host.o"
  nice g++ -std=c++20 -O1 -fno-pie -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" -c "$root/tests/bloom_fast_gl_main.cpp" -o "$o/main.o"
  g++ -no-pie "$o/main.o" "$o/game.o" "$o/bloom.o" "$o/ieu.o" "$o/simple.o" "${objs[@]}" "$out/vgx/host.o" -Wl,--gc-sections -Wl,--allow-multiple-definition -lEGL -lGLESv2 -pthread -lm -o "$exe"
}
# The reference: BloomEffect.cpp / ImageEffectLocalUtil.cpp of BF_BASE (a tree), else of git revision BF_BASE_REV (default caf5e11).
if [[ -z "${BF_BASE:-}" ]]; then
  BF_BASE="$out/basetree"; mkdir -p "$BF_BASE/decomp/src/Game/Screen"
  for f in BloomEffect BloomEffectSimple ImageEffectLocalUtil; do
    git -C "$root" show "${BF_BASE_REV:-caf5e11}:decomp/src/Game/Screen/$f.cpp" > "$BF_BASE/decomp/src/Game/Screen/$f.cpp"
  done
fi
build "$root" "$out/new" "$out/bloom_fast_gl"
build "$BF_BASE" "$out/base" "$out/bloom_base_gl"
echo "built $out"
[[ "${BF_BUILD_ONLY:-0}" == 1 ]] && exit 0

# ---- the runs. GAME: the console's GXV_* defaults (vita/vgx-game/entry.cpp's setenv lines; GXV_PASSHALF=2 among them).
GAME=$(python3 - "$root/vita/vgx-game/entry.cpp" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
env = {}
for m in re.finditer(r'setenv\("([A-Z0-9_]+)",\s*"([^"]*)",\s*0\)', s): env.setdefault(m.group(1), m.group(2))
for m in re.finditer(r'for \(const char\* name : \{([^}]*)\}\) setenv\(name, "([^"]*)", 0\)', s):
    for n in re.findall(r'"([A-Z0-9_]+)"', m.group(1)): env.setdefault(n, m.group(2))
skip = ('GXV_AUTOQ', 'GXV_VITA', 'GXV_FPS', 'GXV_RENDER', 'GXV_SPLIT', 'GXV_CORE', 'GXV_FE_', 'GXV_LOG', 'GXV_BLOOM_FAST')
print(' '.join('%s=%s' % kv for kv in sorted(env.items()) if kv[0].startswith('GXV_') and not kv[0].startswith(skip)))
PY
)
fail=0
run() { # run <tag> <exe> env...
  local tag=$1 exe=$2; shift 2
  rm -rf "$out/d-$tag"; mkdir -p "$out/d-$tag"
  env LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 BF_FRAMES=${BF_FRAMES:-4} "$@" BF_DUMPDIR="$out/d-$tag" "$out/$exe" > "$out/d-$tag/log" 2>&1 ||
    { echo "FAILED: run $tag"; grep -E "FAIL" "$out/d-$tag/log" | head -3; fail=1; }
}
pics() { grep -E '^frame[0-9]+ ' "$out/d-$1/log" | sed -E 's/ scenes=.*//'; }
fifo() { sed -nE 's/^bloom-fifo-hash=([0-9a-f]+).*/\1/p' "$out/d-$1/log"; }
field() { grep -E "^frame$2 " "$out/d-$1/log" | sed -E "s/.* $3=([0-9]+).*/\1/"; }
same() { # same <a> <b> <what>: pictures and the bloom's GX bytes identical
  [[ "$(pics "$1")" == "$(pics "$2")" ]] || { echo "FAILED: $3: pictures differ ($1 / $2)"; fail=1; return; }
  [[ "$(fifo "$1")" == "$(fifo "$2")" ]] || { echo "FAILED: $3: the bloom's GX commands differ ($1 / $2)"; fail=1; return; }
  echo "ok   $3: identical pictures and bloom GX bytes"
}
samepics() { [[ "$(pics "$1")" == "$(pics "$2")" ]] && echo "ok   $3: identical pictures" || { echo "FAILED: $3: pictures differ ($1 / $2)"; fail=1; }; }
for size in "720 404" "960 544" "608 346"; do
  set -- $size; W=$1; H=$2; S="BF_RW=$W BF_RH=$H"
  for prof in plain game; do
    P=""; [[ $prof == game ]] && P=$GAME
    t=$prof$W
    run b$t bloom_base_gl $P $S
    run z$t bloom_fast_gl $P $S GXV_BLOOM_FAST=0
    run f$t bloom_fast_gl $P $S GXV_BLOOM_FAST=1
    run s$t bloom_base_gl $P $S BF_NOGLOW=1
    run bb$t bloom_base_gl $P $S BF_BRIGHT=1
    run fb$t bloom_fast_gl $P $S BF_BRIGHT=1 GXV_BLOOM_FAST=1
    same b$t z$t "$prof ${W}x$H GXV_BLOOM_FAST=0 = base"
    # scenes: the fast chain makes at least 4 texture scenes fewer than the base (608x346: 5, a pass of the base that is not drawn direct there) (frames after the first: the first one registers programs)
    for f in 2 3 4; do
      sb=$(field b$t $f scenes); sf=$(field f$t $f scenes); cb=$(field b$t $f copies); cf=$(field f$t $f copies)
      [[ $sf -le $((sb - 4)) && $cf == $((cb - 4)) ]] || { echo "FAILED: $prof ${W}x$H frame $f: scenes $sb -> $sf, bloom copies $cb -> $cf (expected at least 4 scenes and 4 copies fewer)"; fail=1; }
    done
    echo "     $prof ${W}x$H scenes a picture: base $(field b$t 3 scenes) (EFB $(field b$t 3 efb)), fast $(field f$t 3 scenes) (EFB $(field f$t 3 efb)); bloom copies $(field b$t 3 copies) -> $(field f$t 3 copies)"
    echo "     $prof ${W}x$H all sources: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-b$t" "$out/d-f$t" --scene "$out/d-s$t" | tail -1)"
    echo "     $prof ${W}x$H bright sources: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-bb$t" "$out/d-fb$t" --scene "$out/d-s$t" | tail -1)"
    python3 "$root/tools/bloom_fast_cmp.py" "$out/d-b$t" "$out/d-f$t" --min-psnr "${BF_MIN_PSNR:-22}" > /dev/null || { echo "FAILED: $prof ${W}x$H: a picture under ${BF_MIN_PSNR:-22} dB"; fail=1; }
  done
  echo "     ${W}x$H the console's default chain (GXV_PASSHALF=2) against the plain one: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-bplain$W" "$out/d-bgame$W" | tail -1)"
  echo "     ${W}x$H the fast chain against the plain original: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-bplain$W" "$out/d-fgame$W" | tail -1)"
done
t=game720; G="$GAME BF_RW=720 BF_RH=404"
# no glow source: the chain blurs black, the picture is the scene put back (exact in both chains); PETARI_BLOOM_SKIP's dry chain too
run ng0 bloom_base_gl $G BF_NOGLOW=1; run ng1 bloom_fast_gl $G BF_NOGLOW=1 GXV_BLOOM_FAST=1
samepics ng0 ng1 "no glow source: fast = base"
run sk0 bloom_base_gl $G BF_NOGLOW=1 PETARI_BLOOM_SKIP=1; run sk1 bloom_fast_gl $G BF_NOGLOW=1 PETARI_BLOOM_SKIP=1 GXV_BLOOM_FAST=1
samepics ng0 sk1 "PETARI_BLOOM_SKIP=1 dry chain with the fast chain: = base"
[[ $(field sk1 3 scenes) == $(field sk0 3 scenes) ]] && echo "ok   dry chain: $(field sk1 3 scenes) scenes in both" || { echo "FAILED: dry chain scenes differ"; fail=1; }
# "perf": only in Performance mode (GXV_AUTOQ_TARGET=60 without the 30 cap)
run p0 bloom_fast_gl $G GXV_BLOOM_FAST=perf; run p1 bloom_fast_gl $G GXV_BLOOM_FAST=perf GXV_AUTOQ_TARGET=60; run p2 bloom_fast_gl $G GXV_BLOOM_FAST=perf GXV_AUTOQ_TARGET=60 GXV_FPS_LIMIT=30
samepics bgame720 p0 "perf outside Performance mode = original"; samepics fgame720 p1 "perf in Performance mode = fast"; samepics bgame720 p2 "perf with the 30 cap = original"
# PETARI_BLOOM_HALFRATE: every other picture adds the kept glow (the fast chain's _38 + _34)
run h0 bloom_base_gl $G PETARI_BLOOM_HALFRATE=1; run h1 bloom_fast_gl $G PETARI_BLOOM_HALFRATE=1 GXV_BLOOM_FAST=1
[[ $(field h1 2 scenes) -lt $(field h1 3 scenes) ]] && echo "ok   PETARI_BLOOM_HALFRATE with the fast chain: reuse pictures $(field h1 2 scenes) scenes, chain pictures $(field h1 3 scenes)" || { echo "FAILED: no reuse picture"; fail=1; }
echo "     PETARI_BLOOM_HALFRATE base vs fast: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-h0" "$out/d-h1" | tail -1)"
# GXV_BLOOM_FAST=2 (exact): the normal bloom as the base; the simple bloom without its unused capture (the same pictures, one scene fewer)
run x2 bloom_fast_gl $G GXV_BLOOM_FAST=2; same bgame720 x2 "GXV_BLOOM_FAST=2, normal bloom = base"
# the simple bloom (BloomEffectSimple::draw, SimpleBloom areas): mask filters I8 / R8 / B8
for mk in 0 1 3; do
  run sb$mk bloom_base_gl $G BF_SIMPLE=1 BF_MASK=$mk; run sz$mk bloom_fast_gl $G BF_SIMPLE=1 BF_MASK=$mk GXV_BLOOM_FAST=0
  run se$mk bloom_fast_gl $G BF_SIMPLE=1 BF_MASK=$mk GXV_BLOOM_FAST=2; run sf$mk bloom_fast_gl $G BF_SIMPLE=1 BF_MASK=$mk GXV_BLOOM_FAST=1
  same sb$mk sz$mk "simple bloom mask $mk: GXV_BLOOM_FAST=0 = base"
  samepics sb$mk se$mk "simple bloom mask $mk: GXV_BLOOM_FAST=2 (exact) = base"
  for f in 2 3 4; do
    b=$(field sb$mk $f scenes); e=$(field se$mk $f scenes); q=$(field sf$mk $f scenes); be=$(field sb$mk $f efb); qe=$(field sf$mk $f efb)
    [[ $e == $((b - 1)) && $q == $((b - 3)) && $qe == $((be - 2)) ]] || { echo "FAILED: simple bloom mask $mk frame $f: scenes base $b, exact $e, fast $q (EFB $be -> $qe); expected -1 / -3 (2 EFB splits)"; fail=1; }
  done
  echo "     simple bloom mask $mk: scenes a picture base $(field sb$mk 3 scenes) (EFB $(field sb$mk 3 efb)), exact $(field se$mk 3 scenes), fast $(field sf$mk 3 scenes) (EFB $(field sf$mk 3 efb)); fast: $(python3 "$root/tools/bloom_fast_cmp.py" "$out/d-sb$mk" "$out/d-sf$mk" | tail -1)"
  python3 "$root/tools/bloom_fast_cmp.py" "$out/d-sb$mk" "$out/d-sf$mk" --min-psnr 36 > /dev/null || { echo "FAILED: simple bloom mask $mk under 36 dB"; fail=1; }
done
[[ $fail == 0 ]] && echo "PASS: GXV_BLOOM_FAST=0 is the base (pictures and GX bytes); 1: normal bloom 4 texture scenes fewer, simple bloom 1 texture scene and 2 EFB splits fewer, within the PSNR floors; 2: the same pictures, simple bloom 1 scene fewer; exact without glow / dry; perf and HALFRATE as specified" || { echo FAILED; exit 1; }

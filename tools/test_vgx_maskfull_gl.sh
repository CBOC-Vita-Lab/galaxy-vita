#!/usr/bin/env bash
# GXV_MASKFULL=3 / 5 (hsr24): the clean prefix of a picture. The production front end and GLES back end on Mesa (tests/vgx_maskfull_gl.cpp, the bemerge harness with another
# main). Frames: a copy whose clear leaves the EFB alpha 0, 24 opaque draws with the alpha update OFF, and the final RGBA8 copy (a reader of the alpha); every third frame has a blended draw
# half way that WRITES the alpha, every third one a blended draw that READS the destination alpha. The colour and the alpha of the final picture must be bit-identical between
#   GXV_MASKFULL=0 (the base), 4 (census only), 5 (the full mask in the prefix + the alpha quad), 3 with GXV_FS_ADEAD=1 (the draws whose shader writes alpha 0)
# and the modes must really have acted (draws made full > 0, quads > 0). Negative control: GXV_MASKFULL_NOQUAD=1 (the quad left out) must change the alpha.
#   tools/test_vgx_maskfull_gl.sh [output-dir]      (no game data, no Vita; Khronos headers: KHR_INCLUDE, default /work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
frames=${MASKFULL_FRAMES:-9}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
if [[ "${MASKFULL_SKIP_BUILD:-0}" != 1 ]]; then
  # shellcheck disable=SC2086
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g ${MASKFULL_CXXFLAGS:-} -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_maskfull_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_maskfull_gl"
fi
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
# (fix25) two sets of paths: the plain one (every optional path off) and the console's (Performance: EFBSWAP + EFBSWAP2 hand-overs, the effects runs, BE_MERGE on)
plain=(GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 GXV_EFBSWAP=0 GXV_BE_MERGE=0)
console=(GXV_FXHALF=1 GXV_PASSHALF=2 GXV_CROWD=0 GXV_EFBSWAP=1 GXV_EFBSWAP2=1 GXV_BE_MERGE=1)
paths=("${plain[@]}")
run() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_MASKFULL -u GXV_FS_ADEAD -u GXV_MASKFULL_NOQUAD -u GXV_PASSISO -u GXV_MASKFULL_TESTDROP -u GXV_MASKFULL_TAIL -u GXV_MASKFULL_TAILRUN -u GXV_MASKFULL_FXRA -u GXV_MASKFULL_FIX -u GXV_SS_NOMASK -u GXV_EFBSWAP2 -u MASKFULL_MIDCOPY -u MASKFULL_TAILTEST \
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_AUTOQ=0 GXV_PASSISO=0 "${paths[@]}" \
    "$@" "$out/vgx_maskfull_gl" "$frames" 2>"$out/$tag.err" > "$out/$tag.log" || true
  grep -q -E "FAIL|runtime error|ERROR: AddressSanitizer" "$out/$tag.log" && { echo "FAILED: $tag"; grep -E "FAIL|runtime error|ERROR" "$out/$tag.log" | head -5; exit 1; }
  grep -q '^frame ' "$out/$tag.log" || { echo "FAILED: $tag produced no picture"; tail -5 "$out/$tag.log"; exit 1; }
  grep '^frame ' "$out/$tag.log" > "$out/$tag.frames"
}
field() { sed -n "s/^maskfull .*$2 \([0-9]*\).*/\1/p" "$out/$1.log" | head -1; }
same() { cmp -s "$out/$1.frames" "$out/$2.frames" || { echo "FAILED: $1 and $2 differ"; diff "$out/$1.frames" "$out/$2.frames" | head -6; exit 1; }; }
run base GXV_MASKFULL=0
run m4 GXV_MASKFULL=4
run m5 GXV_MASKFULL=5
run m3 GXV_MASKFULL=3 GXV_FS_ADEAD=1
run m5nq GXV_MASKFULL=5 GXV_MASKFULL_NOQUAD=1
run m5iso GXV_MASKFULL=5 GXV_PASSISO=1   # (the draws go to passes: the mode finds nothing to do; the pictures must still equal the same mode 0 ones)
run baseiso GXV_MASKFULL=0 GXV_PASSISO=1
same base m4; same base m5; same base m3; same baseiso m5iso
[[ $(field m4 made-full) == 0 && $(field m4 quads) == 0 ]] || { echo "FAILED: census mode acted"; exit 1; }
[[ $(field m5 made-full) -gt 0 && $(field m5 quads) -gt 0 ]] || { echo "FAILED: mode 5 did not act: $(grep '^maskfull ' "$out/m5.log")"; exit 1; }
[[ $(field m3 made-full) -gt 0 ]] || { echo "FAILED: mode 3 did not act: $(grep '^maskfull ' "$out/m3.log")"; exit 1; }
[[ $(field m5 foreign) == 0 && $(field m3 foreign) == 0 ]] || { echo "FAILED: foreign alpha masks in the prefix"; exit 1; }
# (mf25) GXV_MASKFULL_FIX=1 / GXV_SS_NOMASK=1: the same pictures, and the modes still act
run m4x GXV_MASKFULL=4 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run m5x GXV_MASKFULL=5 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run m3x GXV_MASKFULL=3 GXV_FS_ADEAD=1 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run m1x GXV_MASKFULL=1 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
same base m4x; same base m5x; same base m3x; same base m1x
[[ $(field m5x made-full) -gt 0 && $(field m5x quads) -gt 0 && $(field m5x foreign) == 0 ]] || { echo "FAILED: mode 5 with GXV_MASKFULL_FIX did not act: $(grep '^maskfull ' "$out/m5x.log")"; exit 1; }
cmp -s "$out/base.frames" "$out/m5nq.frames" && { echo "FAILED: negative control: without the quad the pictures must differ"; exit 1; }

# ---- (fix25) the console's paths, a copy inside the prefix, a plan lost half way, the tail predictor with a picture that needs the quad owed by the one before
paths=("${console[@]}")
run cbase GXV_MASKFULL=0
run c5 GXV_MASKFULL=5
run c5fx GXV_MASKFULL=5 GXV_MASKFULL_FXRA=1
run c4 GXV_MASKFULL=4
run c3 GXV_MASKFULL=3 GXV_FS_ADEAD=1
same cbase c5; same cbase c5fx; same cbase c4; same cbase c3
run c5x GXV_MASKFULL=5 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run c4x GXV_MASKFULL=4 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run c3x GXV_MASKFULL=3 GXV_FS_ADEAD=1 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
run c5xfx GXV_MASKFULL=5 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1 GXV_MASKFULL_FXRA=1 GXV_CORNERPASS=1
same cbase c5x; same cbase c4x; same cbase c3x; same cbase c5xfx
[[ $(field c5x made-full) -gt 0 && $(field c5x foreign) == 0 ]] || { echo "FAILED: console-paths mode 5 with GXV_MASKFULL_FIX did not act: $(grep '^maskfull ' "$out/c5x.log")"; exit 1; }
run cmbase GXV_MASKFULL=0 MASKFULL_MIDCOPY=1
run cm5 GXV_MASKFULL=5 MASKFULL_MIDCOPY=1
same cmbase cm5
run cdrop GXV_MASKFULL=5 GXV_MASKFULL_TESTDROP=3
run cdrop8 GXV_MASKFULL=5 GXV_MASKFULL_TESTDROP=8 MASKFULL_MIDCOPY=1
same cbase cdrop; same cmbase cdrop8
[[ $(field cdrop quads-by-why) != "" ]] || true
run ctbase GXV_MASKFULL=0 MASKFULL_TAILTEST=1
run ct5 GXV_MASKFULL=5 GXV_MASKFULL_TAIL=1 GXV_MASKFULL_TAILRUN=1 MASKFULL_TAILTEST=1
same ctbase ct5
paths=("${plain[@]}")
run tbase GXV_MASKFULL=0 MASKFULL_TAILTEST=1
run t5 GXV_MASKFULL=5 GXV_MASKFULL_TAIL=1 GXV_MASKFULL_TAILRUN=1 MASKFULL_TAILTEST=1
run drop GXV_MASKFULL=5 GXV_MASKFULL_TESTDROP=3
same tbase t5; same base drop
[[ $(field cdrop made-full) -gt 0 && $(field cdrop quads) -gt 0 ]] || { echo "FAILED: console-paths mode 5 did not act: $(grep '^maskfull ' "$out/cdrop.log")"; exit 1; }
[[ $(field c5 foreign) == 0 && $(field cdrop foreign) == 0 ]] || { echo "FAILED: foreign alpha masks with the console paths: $(grep '^maskfull ' "$out/c5.log")"; exit 1; }
why() { grep '^maskfull ' "$out/$1.log" | sed 's/.*quads-by-why//' | awk -v k="$2" '{print $k}'; } # <tag> <1 appointment, 2 safety, 3 carried, 4 plan lost>
[[ $(why drop 4) -gt 0 ]] || { echo "FAILED: the lost plan's quad was not drawn: $(grep '^maskfull ' "$out/drop.log")"; exit 1; }
[[ $(why t5 3) -gt 0 ]] || { echo "FAILED: the tail test never owed a quad to the next picture: $(grep '^maskfull ' "$out/t5.log")"; exit 1; }
echo "maskfull gl, console paths + lost plan + tail carry: identical to mode 0; quads by why (t5) $(grep '^maskfull ' "$out/t5.log" | sed 's/.*quads-by-why//')"
echo "maskfull gl: base == census(4) == mode 5 == mode 3(+ADEAD); mode 5 made-full $(field m5 made-full) quads $(field m5 quads); mode 3 made-full $(field m3 made-full); the NOQUAD control differs"

# ---- (mf26) the game's clear quad carrying the shadow-stencil state (vgx_shadowfill_gl, SF_TRIPLE=1 SF_STENCIL=1 SF_STENCILHASH=1: the window's first draw has stOp ST_ZERO|ST_WINBEGIN, GL test ALWAYS, REPLACE 0).
# GXV_MASKFULL=4 with GXV_MASKFULL_FIX=1 must find the clear (e >= 0) in every picture, without the fix some pictures miss it (control); with GXV_MASKFULL=5 + FIX the pictures (colour, alpha) and the EFB's
# stencil plane at each picture's end are identical to mode 0.
if [[ "${MASKFULL_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g -w ${MASKFULL_CXXFLAGS:-} -I"$src" -I"$root/platform/include" -I"$root/platform/vgx/third_party" -I"$khr" \
    "$root/tests/vgx_shadowfill_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -pthread -o "$out/vgx_shadowfill_gl"
fi
vita="GXV_FIX_DSTA=1 GXV_DSTALPHA_ZERO=1 GXV_NODISCARD=1 GXV_FS_TRIM=1 GXV_VS_TRIM=1 GXV_TEVWRAP=2 GXV_FIX_TEVIN=1 GXV_FS_HALF=1 GXV_FIX_TEXOFF=1
      GXV_FIX_LODBIAS=1 GXV_FIX_LOGICOP=1 GXV_FIX_INDPREV=1 GXV_FOGVTX=2 GXV_LTMERGE=1 GXV_PALROWS=1 GXV_PX_NOOP=1 GXV_PX_DEADCLEAR=1 GXV_PX_SCISSOR=1"
sfrun() { # <tag> env assignments...
  local tag=$1; shift
  env -u GXV_MASKFULL -u GXV_MASKFULL_FIX -u GXV_SS_NOMASK LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 GXV_CROWD=0 SF_TRIPLE=1 SF_STENCIL=1 SF_STENCILHASH=1 \
    $vita GXV_SHADOW_RECT=1 GXV_SHADOW_STENCIL=1 SF_RW=128 SF_RH=128 GXV_MASKFULL_DEBUG=1 "$@" "$out/vgx_shadowfill_gl" > "$out/$tag.log" 2> "$out/$tag.err" || true
  grep -q FAIL "$out/$tag.log" && { echo "FAILED: $tag"; grep FAIL "$out/$tag.log" | head -3; exit 1; }
  grep -E '^(frame|stencil)[0-9]+ hash' "$out/$tag.log" | sed 's/ scenes.*//' > "$out/$tag.pix"
}
found() { grep '^\[maskfull\] picture' "$out/$1.err" | sed 's/.*cmds, e \(-*[0-9]*\) .*/\1/' | awk '$1 >= 0 {n++} END {print n + 0}'; }
total() { grep -c '^\[maskfull\] picture' "$out/$1.err"; }
sfrun sbase GXV_MASKFULL=0
sfrun s4 GXV_MASKFULL=4
sfrun s4f GXV_MASKFULL=4 GXV_MASKFULL_FIX=1
sfrun s5f GXV_MASKFULL=5 GXV_MASKFULL_FIX=1
sfrun s5fx GXV_MASKFULL=5 GXV_MASKFULL_FIX=1 GXV_SS_NOMASK=1
grep -q '^stencil' "$out/sbase.pix" && [[ $(grep '^stencil' "$out/sbase.log" | grep -vc 'nonzero=0$') -gt 0 ]] || { echo "FAILED: the stencil plane was not read, or never set"; exit 1; }
for t in s4 s4f s5f s5fx; do cmp -s "$out/sbase.pix" "$out/$t.pix" || { echo "FAILED: $t differs from mode 0 (colour / alpha / stencil)"; diff "$out/sbase.pix" "$out/$t.pix" | head -6; exit 1; }; done
[[ $(total s4f) -gt 0 && $(found s4f) == $(total s4f) ]] || { echo "FAILED: with GXV_MASKFULL_FIX the stencil clear quad is not found in every picture: $(found s4f) of $(total s4f)"; exit 1; }
[[ $(found s4) -lt $(total s4) ]] || { echo "FAILED: control: without the fix some clear quads must be missed ($(found s4) of $(total s4))"; exit 1; }
echo "maskfull gl, stencil clear quad: mode 4 finds the clear in $(found s4f) of $(total s4f) pictures with the fix ($(found s4) without); modes 4/5 (+FIX, +SS_NOMASK) identical to mode 0 in colour, alpha and stencil"

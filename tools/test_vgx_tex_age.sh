#!/usr/bin/env bash
# texage24: GXV_TEX_SCENEDROP=3 (age and cap), the refetch census and GXV_TEX_ADOPT on the production GX front end and GLES back end (Mesa), no game
# data, no Vita: tests/vgx_bowser_loop_gl.cpp's AGE_SCENARIO (a two-stage loop of scene destructions). Checks the numbers the design promises and that the
# pictures (a hash of the stage's textures as drawn) are identical for modes 0 / 1 / 2 / 3 with and without GXV_TEX_ADOPT.
#   tools/test_vgx_tex_age.sh [output-dir]       (SANITIZE=1: ASan + UBSan; KHR_INCLUDE=/work/khr/include)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
khr=${KHR_INCLUDE:-/work/khr/include}
sources=(vgx_state vgx_draw vgx_fesplit vgx_dlcache vgx_guestmem vgx_bpsync vgx_shadergen vgx_texcache vgx_texdecode vgx_texcomp vgx_hdtex vgx_stats vgx_backend_gl vgx_autoq)
args=()
for unit in "${sources[@]}"; do args+=("$src/$unit.cpp"); done
san=()
[[ "${SANITIZE:-0}" == 1 ]] && san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
if [[ "${AGE_SKIP_BUILD:-0}" != 1 ]]; then
  nice -n 10 "${CXX:-g++}" -std=c++20 -O1 -g "${san[@]}" -I"$src" -I"$root/platform/vgx/third_party" -I"$root/platform/include" -I"$khr" \
    "$root/tests/vgx_bowser_loop_gl.cpp" "${args[@]}" "$root/platform/vgx/host/vgx_host_egl.cpp" \
    -lEGL -lGLESv2 -pthread -o "$out/vgx_tex_age"
fi
fail=0
# run <name> <env...>: prints the AGE-RESULT line's fields as shell assignments in $R_<field>
run() {
  local name=$1; shift
  local line
  line=$(env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=1 GXV_LOG=0 GXV_SPLIT=0 GXV_AUTOQ=0 GXV_FXHALF=0 GXV_PASSHALF=0 \
    GXV_CROWD=0 GXV_SHADER_RECLAIM=1 AGE_SCENARIO=1 "$@" "$out/vgx_tex_age" 2>&1 | grep -E "^AGE-RESULT|FAIL|runtime error|Sanitizer" || true)
  if [[ "$line" != AGE-RESULT* ]] || [[ "$line" == *FAIL* ]] || [[ "$line" == *"runtime error"* ]]; then echo "FAIL: $name: $line"; fail=1; return; fi
  for kv in ${line#AGE-RESULT }; do eval "R_${kv%%=*}=${kv#*=}"; done
  LAST="$line"
}
check() { if (( $1 )); then echo "PASS: $2"; else echo "FAIL: $2 ($LAST)"; fail=1; fi; }
hash0=""
same() { [[ -n "$1" && "$1" == "$2" ]]; }

# a two-stage loop, every stage at fixed addresses (the revisit that finds its textures where it left them), 40 textures a stage
run base-0 GXV_TEX_SCENEDROP=0;                  H0=$R_hash; check "$R_uploads_warm == 0 && $R_cache_end == 80" "mode 0: nothing freed, nothing uploaded again (80 cached)"
run fixed-2 GXV_TEX_SCENEDROP=2;                 check "$R_uploads_warm == 320 && $R_refAddr == 400 && $R_refAddrLive == 400" "mode 2: every visit uploads the stage again (8 visits x 40 after the first loops) and the census counts the refetches in play"
run fixed-3 GXV_TEX_SCENEDROP=3;                 check "$R_uploads_warm == 0 && $R_cache_end == 80 && $R_refAddr == 0" "mode 3 (age 2): a stage idle through one scene keeps its textures: no upload again"
check "$(same $H0 $R_hash && echo 1 || echo 0)" "pictures: mode 3 == mode 0 (fixed addresses)"
run age1 GXV_TEX_SCENEDROP=3 GXV_TEX_SCENEDROP_AGE=1; check "$R_uploads_warm == 320" "mode 3 with age 1 frees what one scene did not bind (as mode 2)"
run three-stage GXV_TEX_SCENEDROP=3 AGE_STAGES=3 AGE_VISITS=18; check "$R_uploads_warm == 480 && $R_refAddr == 600" "mode 3 (age 2), 3 stages: a stage idle through 2 scenes is freed (and counted as refetched)"
run three-stage-a3 GXV_TEX_SCENEDROP=3 AGE_STAGES=3 AGE_VISITS=18 GXV_TEX_SCENEDROP_AGE=3; check "$R_uploads_warm == 0 && $R_cache_end == 120" "mode 3, age 3, 3 stages: kept"
# quiet pictures at a visit's start: the textures are loaded in a quiet picture, not in play
run quiet-2 GXV_TEX_SCENEDROP=2 AGE_QUIET=3;     check "$R_refAddr == 400 && $R_refAddrLive == 0" "census: refetches in a quiet picture are not counted as in play"

# every visit at new addresses with the same bytes (what the stage's archive reloaded gives)
run fresh-0 GXV_TEX_SCENEDROP=0 AGE_FRESH=1 AGE_VISITS=14;  HF=$R_hash; check "$R_cache_end == 560" "fresh addresses, mode 0: the cache grows with every visit (560 entries)"
run fresh-2 GXV_TEX_SCENEDROP=2 AGE_FRESH=1 AGE_VISITS=14;  check "$R_cache_end == 40 && $R_refContent > 0 && $R_refContentLive == $R_refContent" "fresh addresses, mode 2: bounded, the same bytes at another address are counted"
check "$(same $HF $R_hash && echo 1 || echo 0)" "pictures: mode 2 == mode 0 (fresh addresses)"
run fresh-3 GXV_TEX_SCENEDROP=3 AGE_FRESH=1 AGE_VISITS=14;  check "$R_cache_max <= 120 && $R_cache_end <= 120" "fresh addresses, mode 3: bounded (the current and the previous visit, and one more before they are freed)"
check "$(same $HF $R_hash && echo 1 || echo 0)" "pictures: mode 3 == mode 0 (fresh addresses)"
run fresh-3a GXV_TEX_SCENEDROP=3 AGE_FRESH=1 AGE_VISITS=14 GXV_TEX_ADOPT=1; check "$R_uploads_warm == 0 && $R_adopted == 480 && $R_cache_max <= 120" "fresh addresses, mode 3 + GXV_TEX_ADOPT=1: no upload after the first loop, 480 adopted"
check "$(same $HF $R_hash && echo 1 || echo 0)" "pictures: mode 3 + adopt == mode 0 (fresh addresses)"
run fresh-0a GXV_TEX_SCENEDROP=0 AGE_FRESH=1 AGE_VISITS=14 GXV_TEX_ADOPT=1; check "$R_uploads_warm == 0" "adopt alone (mode 0): no upload after the first loop"
check "$(same $HF $R_hash && echo 1 || echo 0)" "pictures: adopt == mode 0"
run fresh-2a GXV_TEX_SCENEDROP=2 AGE_FRESH=1 AGE_VISITS=14 GXV_TEX_ADOPT=1; check "$(same $HF $R_hash && echo 1 || echo 0)" "pictures: mode 2 + adopt == mode 0"

# the destroyed heaps' rule (mode 1's): at once, whatever the age
run heap-3 GXV_TEX_SCENEDROP=3 AGE_FRESH=1 AGE_HEAP=1;  check "$R_cache_end == 0 && $R_cache_max <= 40" "mode 3: textures in the destroyed heaps are freed at once"
run heap-1 GXV_TEX_SCENEDROP=1 AGE_FRESH=1 AGE_HEAP=1;  check "$R_cache_end == 0" "mode 1: the same"

# short scenes between the stages (5 pictures): they must not age the stages' textures
run short-3 GXV_TEX_SCENEDROP=3 AGE_SHORT=5;            HS=$R_hash; check "$R_uploads_warm <= 24 && $R_refAddr == 0" "short scenes do not count as idle scenes (only their own 3 textures a visit are uploaded)"
run short-3off GXV_TEX_SCENEDROP=3 AGE_SHORT=5 GXV_TEX_SCENEDROP_MINFRAMES=0; check "$R_uploads_warm >= 300 && $R_refAddr >= 400" "GXV_TEX_SCENEDROP_MINFRAMES=0: they do (every stage is freed before its next visit)"
check "$(same $HS $R_hash && echo 1 || echo 0)" "pictures: MINFRAMES 30 == 0"
run short-2 GXV_TEX_SCENEDROP=2 AGE_SHORT=5;            check "$(same $HS $R_hash && echo 1 || echo 0)" "pictures: mode 2 == mode 3 (short scenes)"

# the caps: entries, texel KiB; never a texture the destroyed scene bound
run cap-60 GXV_TEX_SCENEDROP=3 GXV_TEX_SCENEDROP_MAX=60;  check "$R_cache_end == 60" "cap of 60 entries: the idle stage is cut oldest first down to 60"
run cap-30 GXV_TEX_SCENEDROP=3 GXV_TEX_SCENEDROP_MAX=30;  check "$R_cache_end == 40" "cap of 30 entries: the 40 the destroyed scene bound are never freed"
run cap-kb GXV_TEX_SCENEDROP=3 GXV_TEX_SCENEDROP_MAXKB=1; check "$R_cache_end == 40" "cap of 1 KiB: likewise"
check "$(same $H0 $R_hash && echo 1 || echo 0)" "pictures: capped == mode 0"
run cap-off GXV_TEX_SCENEDROP=3 GXV_TEX_SCENEDROP_MAX=0 GXV_TEX_SCENEDROP_MAXKB=0; check "$R_cache_end == 80" "caps 0 = no cap"
exit $fail

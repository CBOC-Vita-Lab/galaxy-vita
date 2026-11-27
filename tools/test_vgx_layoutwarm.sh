#!/usr/bin/env bash
# GXV_LAYOUTWARM on the host (layoutwarm24): nw4r::lyt-shaped layouts - the production nw4r::lyt::Material (lyt_material.cpp), detail::SetVertexFormat /
# DrawQuad (lyt_common.cpp) - drawn inside the warm-up scope (PORT_GX_MARK_WARM_BEGIN / _END) through the production GX units and gx_fifo.cpp, parsed by the real
# front end (tests/vgx_layoutwarm.cpp + tests/vgx_prewarm_fe.cpp, the null back end):
#   (a) every program variant a real layout draw is recorded with later was queued by a scope, and no shader is made after the closed frames;
#   (b) every picture with the scopes equals the picture without them (canonical hash: shader ids by key, texture ids by content);
#   (c) the census (GXV_LAYOUTWARM_LOG, kinds 19 / 20): without the warm-up the first draws of layouts log shaders / textures / variants made in play, with
#       it none does ("paid in play" never appears);
#   (d) AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer (GXV_SPLIT=1) builds.
#   tools/test_vgx_layoutwarm.sh [frames] [seeds]      (build in $OUT, default /tmp/vgx_layoutwarm)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-24}
seeds=${2:-"1 2 3 4"}
out=${OUT:-/tmp/vgx_layoutwarm}
mkdir -p "$out"
cxx=${CXX:-g++}
fe=(platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp
    platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp
    platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
inc=(-Iplatform/include -Idecomp/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include -Idecomp/libs/nw4r/include)
build() { # $1: name, $2...: extra flags (sanitizers)
  local name=$1; shift
  local gxo=()
  for u in GXDisplayList GXTev GXTransform GXGeometry GXAttr GXLight GXPixel GXTexture GXBump; do
    ${CC:-clang} -O1 "$@" -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include platform/include/port/compat.h -w \
        -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -c "decomp/src/RVL_SDK/gx/$u.c" -o "$out/$name-$u.o"
    gxo+=("$out/$name-$u.o")
  done
  # (the production nw4r::lyt units, as the game builds them; the platform's compat header is the game's own include and clashes with the host's libstdc++)
  for u in lyt/lyt_material lyt/lyt_common lyt/lyt_texMap lyt/lyt_animation math/math_triangular ut/ut_LinkList; do
    # (texMap and animation are only link fillers: the sanitizers' global registration would keep their unresolved vtables alive, so they stay plain)
    local san=("$@"); case $u in lyt/lyt_texMap|lyt/lyt_animation) san=();; esac
    ${GAME_CXX:-clang++} -std=c++20 -O1 -g "${san[@]}" -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_GX_VGX_ONLY=1 "${inc[@]}" -w \
        -ffunction-sections -fdata-sections -c "decomp/src/nw4r/$u.cpp" -o "$out/$name-${u#*/}.o"
    gxo+=("$out/$name-${u#*/}.o")
  done
  # (the GX capture hooks every GX call of the library carries: the game's own unit, idle without GXV_GXCAPTURE)
  ${GAME_CXX:-clang++} -std=c++20 -O1 -g "$@" -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
      -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 "${inc[@]}" -Iplatform/src/gx -w -ffunction-sections -fdata-sections \
      -include string -include vector -include platform/include/port/compat.h -c platform/src/gx/gx_capture.cpp -o "$out/$name-gxcap.o"
  gxo+=("$out/$name-gxcap.o")
  ${GAME_CXX:-clang++} -std=c++20 -O1 -g "$@" -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
      -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 "${inc[@]}" -Iplatform/src/gx -Wno-register -Wno-c++11-narrowing \
      -Wno-format -Wno-unknown-pragmas -Wno-header-guard -w -ffunction-sections -fdata-sections -c tests/vgx_layoutwarm.cpp -o "$out/$name-game.o"
  $cxx "${common[@]}" -O1 "$@" tests/vgx_prewarm_fe.cpp "${fe[@]}" "$out/$name-game.o" "${gxo[@]}" -Wl,--gc-sections -Wl,--allow-multiple-definition -o "$out/$name"
}
build lw
san=${SAN:-1}
if [ "$san" = 1 ]; then
  build lw-asan -fsanitize=address,undefined -fno-omit-frame-pointer
  build lw-tsan -fsanitize=thread
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1
fail=0
run() { # $1: binary, $2: label, rest: env
  local bin=$1 label=$2; shift 2
  for seed in $seeds; do
    if ! env "$@" LW=0 "$out/$bin" "$frames" "$seed" > "$out/off.txt" 2> "$out/off.err"; then echo "FAIL $label seed $seed: off run"; tail -3 "$out/off.err"; fail=1; continue; fi
    if ! env "$@" LW=1 GXV_LAYOUTWARM=1 "$out/$bin" "$frames" "$seed" > "$out/on.txt" 2> "$out/on.err"; then echo "FAIL $label seed $seed: on run (a)"; grep -E "FAIL|not warmed|ERROR|runtime error" "$out/on.err" | head -5; fail=1; continue; fi
    if ! cmp -s "$out/off.txt" "$out/on.txt"; then echo "FAIL $label seed $seed: (b) pictures differ"; diff "$out/off.txt" "$out/on.txt" | head -4; fail=1; continue; fi
    echo "pass $label seed $seed: $(grep -o '(a) .*' "$out/on.err")"
  done
}
run lw "plain"
run lw "deferred warm list" PW_DEFER=1
run lw "split" GXV_SPLIT=1
run lw "split verify" GXV_SPLIT=2
run lw "rscache verify" GXV_RSCACHE=2
run lw "no rscache" GXV_RSCACHE=0
run lw "no folding" GXV_FOLDREGS=0
run lw "no soft transform" GXV_SOFTXFORM=0
run lw "one layout a frame" LW_SLICE=1 PW_SLICE=1 LW_CLOSED=12
# (the census's markers in the stream change nothing of the pictures either)
run lw "census markers" LW_CENSUS=1 GXV_LAYOUTWARM_LOG=1
if [ "$san" = 1 ]; then
  run lw-asan "asan+ubsan"
  run lw-asan "asan+ubsan split" GXV_SPLIT=1
  run lw-asan "asan+ubsan census" LW_CENSUS=1 GXV_LAYOUTWARM_LOG=1
  run lw-tsan "tsan split" GXV_SPLIT=1
fi
# (c) the census lines: off = paid in play, on = nothing paid
for seed in $seeds; do
  LW=0 LW_CENSUS=1 GXV_LAYOUTWARM_LOG=1 PW_LOGSUM=1 "$out/lw" "$frames" "$seed" > /dev/null 2> "$out/c0.err" || true
  LW=1 GXV_LAYOUTWARM=1 LW_CENSUS=1 GXV_LAYOUTWARM_LOG=1 PW_LOGSUM=1 "$out/lw" "$frames" "$seed" > /dev/null 2> "$out/c1.err" || true
  n0=$(grep -c "layoutwarm: first draw" "$out/c0.err" || true); n1=$(grep -c "layoutwarm: first draw" "$out/c1.err" || true)
  p0=$(grep -c "paid in play" "$out/c0.err" || true); p1=$(grep -c "paid in play" "$out/c1.err" || true)
  if [ "$n0" -lt 1 ] || [ "$n1" -ne "$n0" ] || [ "$p0" -lt 1 ] || [ "$p1" -ne 0 ]; then echo "FAIL (c) seed $seed: census lines off $n0 (paid in play $p0), on $n1 (paid in play $p1)"; fail=1
  else echo "pass (c) seed $seed: $n0 layouts' first draws logged; without the warm-up $p0 paid in play, with it $p1"; fi
done
if [ $fail = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; exit 1; fi

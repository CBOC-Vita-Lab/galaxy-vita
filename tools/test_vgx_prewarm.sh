#!/usr/bin/env bash
# GXV_PREWARM on the host (prewarm24): effects drawn inside the warm-up scope (PORT_GX_MARK_WARM_BEGIN / _END) through the production GX
# units and gx_fifo.cpp, parsed by the real front end (tests/vgx_prewarm.cpp + tests/vgx_prewarm_fe.cpp, the null back end):
#   (a) every program variant a real draw is recorded with later was queued by a scope, and no shader is made after the closed frames;
#   (b) every picture with the scopes equals the picture without them (canonical hash: shader ids by key, texture ids by content);
#   (c) the scopes are spread over the closed frames (PW_SLICE a group a frame) and none is drawn after them (the fixture's gate, as
#       PortPrewarmGroup's port_screen_quiet test) - and the deferred form (PW_DEFER: the variants in the frame's warm list) gives the same;
#   (d) AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer (the front-end split's finishing thread: GXV_SPLIT=1) builds.
#   tools/test_vgx_prewarm.sh [frames] [seeds]      (build in $OUT, default /tmp/vgx_prewarm)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-24}
seeds=${2:-"1 2 3 4"}
out=${OUT:-$MP_TMP/vgx_prewarm}
mkdir -p "$out"
cxx=${CXX:-g++}
fe=(platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp
    platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_texcache.cpp
    platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
cd "$root"
common=(-std=c++17 -g -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread)
inc=(-Iplatform/include -Idecomp/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include)
build() { # $1: name, $2...: extra flags (sanitizers)
  local name=$1; shift
  local gxo=()
  for u in GXDisplayList GXTev GXTransform GXGeometry; do
    ${CC:-clang} -O1 "$@" -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include platform/include/port/compat.h -w \
        -ffunction-sections -fdata-sections -Wno-implicit-function-declaration -c "decomp/src/RVL_SDK/gx/$u.c" -o "$out/$name-$u.o"
    gxo+=("$out/$name-$u.o")
  done
  ${GAME_CXX:-clang++} -std=c++20 -O1 -g "$@" -fno-fast-math -ffp-contract=on -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
      -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 "${inc[@]}" -Iplatform/src/gx -Wno-register -Wno-c++11-narrowing \
      -Wno-format -Wno-unknown-pragmas -Wno-header-guard -ffunction-sections -fdata-sections -c tests/vgx_prewarm.cpp -o "$out/$name-game.o"
  $cxx "${common[@]}" -O1 "$@" tests/vgx_prewarm_fe.cpp "${fe[@]}" "$out/$name-game.o" "${gxo[@]}" -Wl,--gc-sections -Wl,--allow-multiple-definition -o "$out/$name"
}
build pw
san=${SAN:-1}
if [ "$san" = 1 ]; then
  build pw-asan -fsanitize=address,undefined -fno-omit-frame-pointer
  build pw-tsan -fsanitize=thread
fi
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1
fail=0
run() { # $1: binary, $2: label, rest: env
  local bin=$1 label=$2; shift 2
  for seed in $seeds; do
    if ! env "$@" PW=0 "$out/$bin" "$frames" "$seed" > "$out/off.txt" 2> "$out/off.err"; then echo "FAIL $label seed $seed: off run"; tail -3 "$out/off.err"; fail=1; continue; fi
    if ! env "$@" PW=1 GXV_PREWARM=1 "$out/$bin" "$frames" "$seed" > "$out/on.txt" 2> "$out/on.err"; then echo "FAIL $label seed $seed: on run (a)"; grep -E "FAIL|not warmed|ERROR|runtime error" "$out/on.err" | head -5; fail=1; continue; fi
    if ! cmp -s "$out/off.txt" "$out/on.txt"; then echo "FAIL $label seed $seed: (b) pictures differ"; diff "$out/off.txt" "$out/on.txt" | head -4; fail=1; continue; fi
    echo "pass $label seed $seed: $(grep -o '(a) .*' "$out/on.err")"
  done
}
run pw "plain"
run pw "deferred warm list" PW_DEFER=1
run pw "split" GXV_SPLIT=1
run pw "split verify" GXV_SPLIT=2
run pw "rscache verify" GXV_RSCACHE=2
run pw "no rscache" GXV_RSCACHE=0
run pw "no folding" GXV_FOLDREGS=0
run pw "no soft transform" GXV_SOFTXFORM=0
# (GXV_FE_PARALLEL is not compared: with it this fixture's pictures differ from run to run already without the scopes - the decodes' timing
# reaches bytes the hash reads -, see REPORT)
run pw "one scope a frame" PW_SLICE=1 PW_CLOSED=12
if [ "$san" = 1 ]; then
  run pw-asan "asan+ubsan"
  run pw-asan "asan+ubsan split" GXV_SPLIT=1
  run pw-tsan "tsan split" GXV_SPLIT=1
fi
# GXV_PREWARM=0 with the game's scopes in the stream (a mismatch the game avoids by asking port_prewarm_mode): the markers are ignored and the
# scopes' draws are drawn - shown here only to prove the switch is what decides (the pictures must differ)
if PW=1 GXV_PREWARM=0 "$out/pw" "$frames" 1 > "$out/mm.txt" 2> /dev/null; then :; fi
PW=0 "$out/pw" "$frames" 1 > "$out/off.txt" 2> /dev/null || true
if cmp -s "$out/off.txt" "$out/mm.txt"; then echo "FAIL: with GXV_PREWARM=0 the scopes' draws should have been drawn"; fail=1; else echo "pass: GXV_PREWARM=0 ignores the markers (the scopes are drawn: the game must not open them then)"; fi
if [ $fail = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; exit 1; fi

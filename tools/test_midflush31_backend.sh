#!/usr/bin/env bash
# midflush31: GXV_MIDFLUSH's modes in the real back end on the GL stub (tests/midflush31_backend.cpp), built as the console builds it (__VGX_VITA__,
# PORT_RUNTIME_DIAGNOSTICS=0; the harness of tools/test_keepdepth31_backend.sh). One process a case (the switch is read once). The DIGEST (every state call, each draw's
# program and uniform values, in order) and the draw-stream hash must be the same with the switch off and in every mode: a flush changes nothing but where the GPU is handed work.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/midflush31-be.XXXXXX")
trap 'rm -rf "$out"' EXIT
cxx=${CXX_BACKEND:-clang++}
flags=(-std=c++17 -O1 -DNDEBUG -w -D__VGX_VITA__=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/dma31_glshim.h" -I"$root/armhf/glstub"
       -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/tests")
nice -n 10 "$cxx" "${flags[@]}" "$root/tests/midflush31_backend.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" "$root/platform/vgx/src/vgx_autoq.cpp" \
    -o "$out/be" -lpthread
fail=0
declare -A R
run() { # name value [fail]
    local o
    o=$(GXV_LOG=0 "$out/be" "$2" ${3:-}) || { echo "midflush31 case $1: FAIL (exit)"; fail=1; }
    R[$1]=$(grep -E '^(RESULT|CENSUS)' <<<"$o" | tr '\n' ' ')
    echo "$1: ${R[$1]#RESULT }"
    if [[ -n "${V:-}" ]]; then { grep '^LINE' <<<"$o" || true; } | tail -2 | cut -c1-1200; fi
}
field() { sed -n "s/.* $2=\([^ ]*\).*/\1/p" <<<"${R[$1]}"; }
expect() { # what ok
    if [[ "$2" == 1 ]]; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
}
run off 0
run fixed 16
run bound 10064
run bound-small 10016
run idx 20002
run adapt 30003
run adapt1 30001
run failing 16 fail
MFT_CENSUS_MS=0 run census-off 0
MFT_CENSUS_MS=0 run census-on 10064
base=$(field off digest); bh=$(field off streamhash)
expect "off: no flush" $([[ $(field off flushes) == 0 && $(field off stubcalls) == 0 && $(field off on) == 0 ]] && echo 1 || echo 0)
for k in fixed bound bound-small idx adapt adapt1 failing; do
    expect "$k: digest equals the switch-off digest (same state calls, programs, uniforms, draws, in the same order)" $([[ $(field $k digest) == "$base" ]] && echo 1 || echo 0)
    expect "$k: draw-stream hash equals the switch-off hash" $([[ $(field $k streamhash) == "$bh" ]] && echo 1 || echo 0)
    expect "$k: same draw count" $([[ $(field $k draws) == $(field off draws) ]] && echo 1 || echo 0)
done
# 480 draws measured (two pictures)
expect "fixed 16: a flush every 16 draws (30 in the two pictures)" $([[ $(field fixed flushes) == 30 && $(field fixed fixed) == 30 && $(field fixed boundary) == 0 ]] && echo 1 || echo 0)
expect "fixed 16: counted, and each followed by a draw that gave its program" $([[ $(field fixed stubcalls) == 30 && $(field fixed reissued) -ge 28 ]] && echo 1 || echo 0)
expect "bound 64: flushes happen (3 or more) and every one is at a boundary or forced" $([[ $(field bound flushes) -ge 3 && $(( $(field bound boundary) + $(field bound forced) )) == $(field bound flushes) ]] && echo 1 || echo 0)
expect "bound 64: almost all at a change of program (the draw after the flush has another program), and only forced ones do not" $([[ $(field bound after_same) == $(field bound forced) && $(field bound boundary) -ge 3 ]] && echo 1 || echo 0)
expect "bound 16: more flushes than 64, none of them on a draw that keeps the program unless forced" $([[ $(field bound-small flushes) -gt $(field bound flushes) && $(field bound-small after_same) == $(field bound-small forced) ]] && echo 1 || echo 0)
expect "idx 2 (2000 indices): flushes at boundaries" $([[ $(field idx flushes) -ge 1 && $(field idx after_same) == $(field idx forced) ]] && echo 1 || echo 0)
expect "adapt 3: about 3 flushes a picture after the learning pictures, at boundaries" $([[ $(field adapt flushes) -ge 4 && $(field adapt flushes) -le 6 && $(field adapt after_same) == $(field adapt forced) ]] && echo 1 || echo 0)
expect "adapt 1: about 1 flush a picture" $([[ $(field adapt1 flushes) -ge 2 && $(field adapt1 flushes) -le 3 ]] && echo 1 || echo 0)
expect "a failing sceGxmMidSceneFlush: counted once, then the switch is off, nothing else changes" $([[ $(field failing failed) == 1 && $(field failing on) == 0 && $(field failing flushes) == 0 ]] && echo 1 || echo 0)
expect "census lines: off prints the ring line only (GXV_RING_CENSUS=1 asked for), a mode prints both" $([[ $(field census-off midflush_lines) == 0 && $(field census-off ring_lines) -ge 1 && $(field census-on midflush_lines) -ge 1 && $(field census-on ring_lines) -ge 1 ]] && echo 1 || echo 0)
expect "census run: the same digest (the census changes nothing)" $([[ $(field census-on digest) == "$base" ]] && echo 1 || echo 0)
[[ $fail -eq 0 ]] || exit 1
echo "midflush31 back end: PASS"

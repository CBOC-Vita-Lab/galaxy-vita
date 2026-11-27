#!/usr/bin/env bash
# GXV_PAD_LEAN (thr24): the real vita/runtime/input.cpp with stub system calls; the same 4000 scripted polls with the switch off and on give the same pad states
# (the tilt fields only differ outside tilt levels: compared blind to them) and far fewer system calls.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/pad-lean.XXXXXX")}
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk vitasdk.h; do [ -e "$out/inc/$d" ] || { [ ! -e "$sdk/arm-vita-eabi/include/$d" ] || ln -s "$sdk/arm-vita-eabi/include/$d" "$out/inc/$d"; }; done
"${CXX:-clang++}" -std=c++20 -O1 -g -fno-short-enums -DPORT_RUNTIME_DIAGNOSTICS=0 -DTARGET_PC -isystem "$out/inc" -I"$root/platform/include" -I"$root/vita/runtime" -I"$root/decomp/libs/RVL_SDK/include" \
    "$root/tests/pad_lean_test.cpp" "$root/vita/runtime/input.cpp" "$root/platform/src/input/control_bindings.cpp" -o "$out/pad_lean"
get() { echo "$1" | sed -E "s/.*$2 ([0-9a-f]+).*/\1/"; }
for remap in 0 1; do
    off=$(GXV_REMAP=$remap GXV_PAD_LEAN=0 "$out/pad_lean"); on=$(GXV_REMAP=$remap GXV_PAD_LEAN=1 "$out/pad_lean")
    echo "remap=$remap off: $off"; echo "remap=$remap on:  $on"
    [[ $(get "$off" digest) != "" ]] || exit 1
    [[ $(echo "$off" | sed -E 's/.* blind ([0-9a-f]+).*/\1/') == $(echo "$on" | sed -E 's/.* blind ([0-9a-f]+).*/\1/') ]] || { echo "FAIL: pad states differ with the switch on (remap $remap)" >&2; exit 1; }
    p0=$(get "$off" port-queries); p1=$(get "$on" port-queries); r0=$(get "$off" rear-reads); r1=$(get "$on" rear-reads); m0=$(get "$off" motion-reads); m1=$(get "$on" motion-reads)
    (( r1 < r0 && m1 < m0 )) || { echo "FAIL: rear / motion reads did not drop ($r0 -> $r1, $m0 -> $m1)" >&2; exit 1; }
    if [[ $remap == 1 ]]; then (( p0 >= 4000 && p1 * 40 <= p0 )) || { echo "FAIL: port queries did not drop ($p0 -> $p1)" >&2; exit 1; }; fi
    [[ $remap == 0 || $(get "$on" after-switch-queries) -ge 1 ]] || { echo "FAIL: a changed port was not seen" >&2; exit 1; }
done
echo "pad lean: identical pad states, fewer system calls, a changed controller is seen"

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/slip-trace.XXXXXX")}
mkdir -p "$out"
"${NODE:-node}" "$root/tools/prepare_slip_trace_fixture.cjs" "$root" "$out/slip_trace_methods.inc"
test -s "$out/slip_trace_methods.inc"
for variant in vita nonvita silent vita-silent; do
    defines=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1)
    case "$variant" in
        vita) defines+=(-D__vita__);;
        silent) defines=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0);;
        vita-silent) defines=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -D__vita__);;
    esac
    "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-pointer-bool-conversion -fsanitize=address,undefined -fno-omit-frame-pointer \
        "${defines[@]}" -I"$root/platform/include" -I"$out" "$root/tests/slip_trace.cpp" -o "$out/slip-trace-$variant"
    effective=0
    if test "$variant" = vita; then effective=1; fi
    GXV_SLIP=1 GXV_SLIP_MAX=20 GXV_SLIP_TRACE=1 "$out/slip-trace-$variant" 20 "$effective" config
    for setting in unset '' 0 2 -1 invalid 01 0x1 1suffix ' 1' '1 '; do
        if test "$setting" = unset; then unset GXV_SLIP_TRACE; else export GXV_SLIP_TRACE="$setting"; fi
        GXV_SLIP=1 GXV_SLIP_MAX=20 "$out/slip-trace-$variant" 20 0 config
    done
    for cap in unset -5 0 1 20 50 99; do
        expected=$cap
        case "$cap" in unset) unset GXV_SLIP_MAX; expected=15;; -5|0) export GXV_SLIP_MAX="$cap"; expected=1;; 99) export GXV_SLIP_MAX="$cap"; expected=50;; *) export GXV_SLIP_MAX="$cap";; esac
        GXV_SLIP=1 GXV_SLIP_TRACE=1 "$out/slip-trace-$variant" "$expected" "$effective" config
    done
    unset GXV_SLIP_MAX
    for slip in unset 0; do
        if test "$slip" = unset; then unset GXV_SLIP; else export GXV_SLIP=0; fi
        GXV_SLIP_TRACE=1 "$out/slip-trace-$variant" 0 "$effective" config
    done
    for scenario in turns boundary-inside boundary-outside forbidden; do
        for trace in 0 1; do
            active=0
            if test "$trace" = 1; then active=$effective; fi
            GXV_SLIP=1 GXV_SLIP_MAX=20 GXV_SLIP_TRACE="$trace" "$out/slip-trace-$variant" 20 "$active" "$scenario" > "$out/$variant-$scenario-trace$trace.samples"
        done
        cmp "$out/$variant-$scenario-trace0.samples" "$out/$variant-$scenario-trace1.samples"
    done
    GXV_SLIP=0 GXV_SLIP_MAX=20 GXV_SLIP_TRACE=1 "$out/slip-trace-$variant" 0 "$effective" turns > "$out/$variant-disabled.samples"
done

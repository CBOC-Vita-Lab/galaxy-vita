#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cd "$root"
flags=(-O2 -g -fno-fast-math -ffp-contract=on -fno-strict-aliasing
    -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -fms-extensions
    -Iplatform/include -Iplatform/src/port -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include -Idecomp/include
    -Wno-register -Wno-c++11-narrowing -Wno-format -ffunction-sections -fdata-sections)
link=(-Wl,--gc-sections)
if test "$(uname -s)" = Darwin; then link=(-Wl,-dead_strip); fi
mkdir -p "$out/baseline"
if test -n "${PHASE_CPU_BASELINE_PERF:-}"; then
    test -f "$PHASE_CPU_BASELINE_PERF"
    cp "$PHASE_CPU_BASELINE_PERF" "$out/baseline/perf.cpp"
else
    # Final 1.2 already includes workload/slip hooks; isolate phase changes from those additions.
    git show fc78e5e3a50ab2bc7fb52867796d66a8be278da2:platform/src/port/perf.cpp > "$out/baseline/perf.cpp"
fi
for mode in diagnostic quiet silent vita-quiet vita-silent armhf-gate; do
    define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1)
    case "$mode" in
        quiet) define=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1);;
        silent) define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=0);;
        vita-quiet) define=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 -D__vita__);;
        vita-silent) define=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -D__vita__);;
        armhf-gate) define+=(-DPORT_ARMHF_HARNESS);;
    esac
    "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" -fsanitize=address,undefined \
        tests/phase_cpu.cpp "${link[@]}" -pthread -o "$out/test-$mode"
    if test "$mode" = diagnostic || test "$mode" = vita-quiet; then
        # On first: a missing production observer must fail behavior, not compilation.
        "$out/test-$mode" 1 1 basic
        for setting in unset 0 2 -1 invalid 9999999999999999999999999999999999; do "$out/test-$mode" "$setting" 0 basic; done
        for setting in 0x1 1suffix; do "$out/test-$mode" "$setting" 1 basic; done
        for scenario in faults matched endpoints boundaries threads all-phases; do "$out/test-$mode" 1 1 "$scenario"; done
        if test "$mode" = vita-quiet; then
            for setting in unset 0 2 -1 invalid; do "$out/test-$mode" "$setting" 0 disabled; done
            if test "$(uname -s)" = Linux; then
                "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" -DPHASE_CPU_TEST_EMUTLS=1 \
                    -femulated-tls -fsanitize=address,undefined tests/phase_cpu.cpp "${link[@]}" \
                    -Wl,--wrap=__emutls_get_address -pthread -o "$out/test-vita-emutls"
                for setting in unset 0 2 -1 invalid; do "$out/test-vita-emutls" "$setting" 0 disabled; done
                "$out/test-vita-emutls" 1 1 emutls-errno
            fi
        fi
    else
        "$out/test-$mode" 1 0 basic
        nm "$out/test-$mode" > "$out/$mode-symbols.txt"
        if grep -q port_native_phase_cpu_checkpoint "$out/$mode-symbols.txt"; then
            echo 'FAIL compiled-out observer symbol' >&2; exit 1
        fi
        for name in current baseline; do
            source=platform/src/port/perf.cpp
            if test "$name" = baseline; then source="$out/baseline/perf.cpp"; fi
            "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" -g0 -S "$source" -o "$out/$mode-$name.s"
            sed '/^[[:space:]]*\.file[[:space:]]/d' "$out/$mode-$name.s" > "$out/$mode-$name.normalized.s"
        done
        cmp "$out/$mode-current.normalized.s" "$out/$mode-baseline.normalized.s"
        printf 'PASS %s full perf assembly matches baseline after filename-directive removal\n' "$mode"
    fi
done

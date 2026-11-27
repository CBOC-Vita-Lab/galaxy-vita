#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
cd "$root"
flags=(-O2 -g -fno-fast-math -ffp-contract=on -fno-strict-aliasing
    -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -fms-extensions
    -Iplatform/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/JSystem/include -Idecomp/include
    -Wno-register -Wno-c++11-narrowing -Wno-format -ffunction-sections -fdata-sections)
link=()
if test "$(uname -s)" = Darwin; then link=(-Wl,-dead_strip); else link=(-Wl,--gc-sections); fi
git show 392ee7009fe1152b5bd397e6398f00404303a264:platform/src/sdk/mtx.c > "$out/baseline-mtx.c"
for mode in diagnostic quiet silent vita-gate; do
    define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1)
    if test "$mode" = quiet; then define=(-DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1); fi
    if test "$mode" = silent; then define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=0); fi
    if test "$mode" = vita-gate; then define=(-DPORT_RUNTIME_DIAGNOSTICS=1 -DPORT_LOGGING=1 -D__vita__); fi
    "${CC:-clang}" -std=c11 "${flags[@]}" "${define[@]}" -fsanitize=address,undefined \
        -include platform/desktop/c_compat.h -include platform/include/port/compat.h \
        -c platform/src/sdk/mtx.c -o "$out/mtx-$mode.o"
    "${CC:-clang}" -std=c11 "${flags[@]}" "${define[@]}" -fsanitize=address,undefined \
        -include platform/desktop/c_compat.h -include platform/include/port/compat.h \
        -c tests/mtx_profile_reference.c -o "$out/reference-$mode.o"
    "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" -fsanitize=address,undefined \
        tests/mtx_profile.cpp "$out/mtx-$mode.o" "$out/reference-$mode.o" "${link[@]}" -pthread -o "$out/test-$mode"
    if test "$mode" = diagnostic; then
        for setting in unset 0 2 -1 invalid 9999999999999999999999999999999999; do "$out/test-$mode" "$setting" 0; done
        for setting in 1 0x1 1suffix; do "$out/test-$mode" "$setting" 1; done
        "${CXX:-clang++}" -std=c++20 "${flags[@]}" "${define[@]}" -fsanitize=address,undefined \
            tests/mtx_profile_init.cpp "$out/mtx-$mode.o" "$out/reference-$mode.o" "${link[@]}" -pthread -o "$out/test-init"
        for setting in 0 invalid 9999999999999999999999999999999999; do "$out/test-init" "$setting" 0; done
        "$out/test-init" 1 1
    else
        "$out/test-$mode" 1 0
        nm "$out/mtx-$mode.o" "$out/test-$mode" > "$out/$mode-symbols.txt"
        if grep -q port_mtx_profile "$out/$mode-symbols.txt"; then echo 'FAIL: compiled-out matrix/perf contains a profile symbol' >&2; exit 1; fi
        for source in platform/src/sdk/mtx.c "$out/baseline-mtx.c"; do
            name=$(basename "$source")
            "${CC:-clang}" -std=c11 "${flags[@]}" "${define[@]}" -g0 \
                -include platform/desktop/c_compat.h -include platform/include/port/compat.h \
                -S "$source" -o "$out/$mode-$name.s"
            sed '/^[[:space:]]*\.file[[:space:]]/d' "$out/$mode-$name.s" > "$out/$mode-$name.normalized.s"
        done
        cmp "$out/$mode-mtx.c.normalized.s" "$out/$mode-baseline-mtx.c.normalized.s"
        printf 'PASS: %s matrix assembly matches pinned baseline exactly after filename-directive removal\n' "$mode"
    fi
done

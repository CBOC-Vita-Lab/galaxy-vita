#!/usr/bin/env bash
# GXV_AUDIO_THREAD: the DSP mixer on its own thread gives the same PCM, voice blocks and mail order as the mixer inside the release.
# Builds tests/dsp_async_mix.cpp with the real ai_dsp.cpp and dsp_mixer.cpp (address+UB sanitizers, then ThreadSanitizer with the
# diagnostics counters on), runs the scripted command sequence with the switch off, on, unset and with unusable values, and compares
# the digests.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dsp-async.XXXXXX")}
mkdir -p "$out"
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
flags=(-std=c++20 -fwrapv -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1
    -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
    -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/nw4r/include" -I"$root/decomp/libs/RVLFaceLib/include"
    -include string -include "$root/platform/include/port/compat.h" -Wno-register -Wno-unknown-pragmas -pthread)
srcs=("$root/tests/dsp_async_mix.cpp" "$root/platform/src/audio/ai_dsp.cpp" "$root/platform/src/audio/dsp_mixer.cpp")
build() {  # name, sanitizer, diagnostics
    local objs=()
    for s in "${srcs[@]}"; do
        local o="$out/$1-$(basename "$s" .cpp).o"
        "${CXX:-clang++}" "${flags[@]}" -DPORT_RUNTIME_DIAGNOSTICS="$3" -O2 -g "-fsanitize=$2" -fno-omit-frame-pointer -c "$s" -o "$o"
        objs+=("$o")
    done
    "${CXX:-clang++}" -pthread "-fsanitize=$2" "${objs[@]}" -o "$out/$1"
}
check() {  # binary
    local ref=""
    for mode in 0 1 unset garbage 2 -1 ''; do
        if [[ $mode == unset ]]; then env -u GXV_AUDIO_THREAD "$1" "$out/digest-$mode" >/dev/null
        else GXV_AUDIO_THREAD=$mode "$1" "$out/digest-$mode" >/dev/null; fi
        [[ -z $ref ]] && ref=$(cat "$out/digest-$mode")
        [[ $(cat "$out/digest-$mode") == "$ref" ]] || { echo "FAIL: digest with GXV_AUDIO_THREAD=$mode differs from the first run" >&2; exit 1; }
    done
    GXV_AUDIO_THREAD=1 "$1" | tee "$out/last-on"
    GXV_AUDIO_THREAD=0 "$1" | tee "$out/last-off"
}
build asan address,undefined 0
check "$out/asan"
build tsan thread 1
for i in 1 2 3; do check "$out/tsan"; done
echo "dsp async mix: identical digests with the switch off and on, ASan/UBSan and TSan clean"

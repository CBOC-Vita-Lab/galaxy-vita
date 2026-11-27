#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=codex-galaxy-native:20261007-arm64
compiler=clang++
flags='-stdlib=libc++ -O1 -g -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer'
libs='-l:libasan.so.8 -l:libubsan.so.1'
run=(env)
mounts=(--mount "type=bind,source=$root,target=/work,readonly")
fixture=0
if [[ -n ${HOST_CLOCK_FIXTURE_DIR:-} ]]; then
    mounts+=(--mount "type=bind,source=$HOST_CLOCK_FIXTURE_DIR,target=/fixture,readonly")
    fixture=1
fi
if [[ ${1:-} == arm32 ]]; then
    image=codex-strikers-arm-tests:20261006
    compiler=arm-linux-gnueabihf-g++
    flags='-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-fast-math -Wno-psabi'
    libs=''
    run=(qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    "${mounts[@]}" \
    --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
    -e "CXX=$compiler" -e "FLAGS=$flags" -e "LIBS=$libs" -e "CLOCK_NATIVE=${CLOCK_NATIVE:-0 1}" \
    -e "CLOCK_FIXTURE=$fixture" \
    "$image" bash -euo pipefail -c '
        export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
        common=(-std=c++20 -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions -DPORT_ARMHF_HARNESS=1
            -DPORT_LOGGING=0 -DPORT_PLATFORM_CODE=1
            -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime
            -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
            -include tests/vita_host_clock_prelude.h -Wno-register -Wno-unknown-pragmas
            -Wno-deprecated-declarations)
        for native in $CLOCK_NATIVE; do for diagnostics in 0 1; do
            extra=()
            if test "$native" = 1; then extra=(-D__vita__); fi
            if test "$CLOCK_FIXTURE" = 1; then extra+=(-DHOST_CLOCK_FIXTURE -I/fixture); fi
            $CXX $FLAGS "${common[@]}" "${extra[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics \
                tests/vita_host_clock.cpp -o /tmp/host-clock $LIBS
            for mode in real virtual; do timeout 30s "$@" /tmp/host-clock "$mode"; done
        done; done
    ' -- "${run[@]}"

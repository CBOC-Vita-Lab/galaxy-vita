#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=codex-galaxy-native:20261007-arm64
if test "${GAP_ARM32:-0}" = 1; then image=codex-strikers-arm-tests:20261006; fi
mounts=(--mount "type=bind,source=$root,target=/work,readonly"
        --mount "type=bind,source=$sdk,target=/sdk,readonly")
if test -n "${VITA_GAP_FIXTURE_DIR:-}"; then
  test -f "$VITA_GAP_FIXTURE_DIR/os_thread.cpp"
  mounts+=(--mount "type=bind,source=$VITA_GAP_FIXTURE_DIR,target=/fixture,readonly")
fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -e "GAP_ARM32=${GAP_ARM32:-0}" "${mounts[@]}" -w /work \
  "$image" bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    common=(-std=c++20 -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
      -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=getenv -Iplatform/include -Ivita/runtime
      -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
      -include tests/vita_gap_prelude.h -Wno-register -Wno-unknown-pragmas
      -Wno-deprecated-declarations)
    if test -d /fixture; then common+=(-DVITA_THREAD_SOURCE=\"/fixture/os_thread.cpp\"); fi
    if test "$GAP_ARM32" = 1; then
      compiler=(arm-linux-gnueabihf-g++)
      common+=(-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -pthread -Wno-volatile -Wno-psabi)
      libs=(-lm)
      execute=(timeout 30s qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf)
    else
      compiler=(clang++ -stdlib=libc++)
      common+=(-Wno-deprecated-volatile -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer)
      libs=(-l:libasan.so.8 -l:libubsan.so.1)
      execute=(timeout 10s)
    fi
    for diagnostic in 0 1; do
      for logging in 0 1; do
        "${compiler[@]}" "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostic -DPORT_LOGGING=$logging \
          tests/vita_gap_diagnostics.cpp -o /tmp/gap "${libs[@]}"
        for mode in both gap profile off; do "${execute[@]}" /tmp/gap "$mode"; done
      done
    done
  '

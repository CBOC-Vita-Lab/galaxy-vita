#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
vgl='/opt/galaxy-vgl'
mkdir -p "$root/local/host-bookkeeping-tests"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/host-bookkeeping-tests,target=/results" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" \
  --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    for flags in 0,0,0 0,1,0 1,1,0 0,0,1 0,1,1 1,1,1; do
      IFS=, read -r diagnostics logging trace <<< "$flags"
      clang++ -std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -D__VGX_VITA__ -DNOMINMAX \
        -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics -DPORT_LOGGING=$logging -DVGX_COLDTRACE=$trace \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=pthread_create \
        -Iplatform/include -Iplatform/vgx/vita/include -I/vgl/include -Iplatform/vgx/src \
        -Iplatform/vgx/third_party -idirafter /sdk/arm-vita-eabi/include \
        -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/vita_host_bookkeeping.cpp -o /results/host-$diagnostics-$logging-$trace -l:libasan.so.8 -l:libubsan.so.1
      timeout 20s /results/host-$diagnostics-$logging-$trace
    done
  '

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
vgl='/opt/galaxy-vgl'
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$sdk,target=/sdk,readonly" \
  --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work -e "SEED_CASE=${1:-}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    for probe in 0 1; do
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -D__VGX_VITA__ -DNOMINMAX -DVGX_COLDTRACE=$probe \
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Iplatform/include -Iplatform/vgx/vita/include -I/vgl/include -Iplatform/vgx/src \
      -Iplatform/vgx/third_party -idirafter /sdk/arm-vita-eabi/include \
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vita_seed_cache.cpp -o /tmp/seed-cache -l:libasan.so.8 -l:libubsan.so.1
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    if test -n "$SEED_CASE"; then timeout 30s /tmp/seed-cache "$SEED_CASE"; continue; fi
    for mode in equal-size copy-short-read copy-read-error copy-close-error copy-short-write same changed dir-error cap cap-exact partial-list stamp-read-error stamp-close-error legacy marker-nul marker-short-write zero-gxp existing-gxp adopt-read-error adopt-write-error adopt-close-error bucket-early bucket-lengths list-stat-error adopt-stat-error adopt-missing adopt-retry adopt-crash-before-list adopt-large stamp-known stamp-chunks stamp-io-count list-override adopt-close-retry adopt-restore-fail adopt-marker-fail adopt-preread-close-error; do
      timeout 30s /tmp/seed-cache "$mode"
    done
    done
  '

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/local/variant-hash-tests"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$root/local/variant-hash-tests,target=/results" -w /work \
  codex-strikers-arm-tests:20261006 bash -euo pipefail -c '
    arm-linux-gnueabihf-g++ -std=c++17 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
      -DVGX_NO_NEON -Wno-psabi -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Iplatform/vgx/src -Iplatform/vgx/third_party tests/vgx_variant_hash.cpp -o /results/variant-arm32
    timeout 30s qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf /results/variant-arm32 "${1:---strict}"
  ' -- "${1:---strict}"

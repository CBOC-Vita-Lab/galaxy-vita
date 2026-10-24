#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d "${TMPDIR:-/tmp}/vgx-csv-status.XXXXXX")
trap 'rm -rf "$out"' EXIT
node "$root/tools/extract_vgx_csv_status.cjs" "$root" "$out"
docker run --rm --pull=never --platform linux/amd64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$out,target=/fixture" \
  --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
  -e "CSV_STATUS_DEBUG=${CSV_STATUS_DEBUG:-0}" \
  codex-galaxy-native:20261007 bash -euo pipefail -c '
    cp /sdk/arm-vita-eabi/lib/libc.a /fixture/libc-formatter.a
    /sdk/bin/arm-vita-eabi-ar d /fixture/libc-formatter.a libc_a-syscalls.o libc_a-io.o libc_a-threading.o libc_a-mlock.o libc_a-sbrk.o
    /sdk/bin/arm-vita-eabi-g++ -std=c++17 -O2 -mcpu=cortex-a9 -marm -fomit-frame-pointer \
      -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -nostdlib -Wl,-e,_start,--gc-sections -I/fixture \
      tests/vita_csv_status.cpp -o /fixture/csv-status /fixture/libc-formatter.a -lm -lgcc
    if [[ $CSV_STATUS_DEBUG == 1 ]];then /sdk/bin/arm-vita-eabi-readelf -h -l /fixture/csv-status;fi
  '
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$out,target=/fixture,readonly" \
  -e "CSV_STATUS_DEBUG=${CSV_STATUS_DEBUG:-0}" \
  codex-strikers-arm-tests:20261006 bash -euo pipefail -c '
    extra=()
    if [[ $CSV_STATUS_DEBUG == 1 ]];then extra=(-strace);fi
    timeout 20s qemu-arm "${extra[@]}" -cpu cortex-a9 /fixture/csv-status
  '

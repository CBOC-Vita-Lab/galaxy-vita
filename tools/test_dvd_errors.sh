#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra \
      -fsanitize=undefined -fsanitize-trap=undefined -fms-extensions \
      -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
      -include platform/include/port/compat.h -Wno-ignored-qualifiers \
      -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function -Wno-frame-address \
      -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      tests/dvd_errors.cpp platform/src/os/os_alarm.cpp -lpthread -o /tmp/dvd_errors
    GXV_DVDERRORS=1 /tmp/dvd_errors enabled > /tmp/enabled.txt 2> /tmp/enabled-errors.txt
    GXV_DVDERRORS=0 /tmp/dvd_errors disabled > /tmp/disabled.txt
    env -u GXV_DVDERRORS /tmp/dvd_errors disabled > /tmp/default.txt
    GXV_DVDERRORS=invalid /tmp/dvd_errors disabled > /tmp/invalid.txt
    GXV_DVDERRORS=0x1 /tmp/dvd_errors enabled > /tmp/hex.txt 2> /tmp/hex-errors.txt
    for mode in disabled default invalid hex; do cmp /tmp/enabled.txt "/tmp/$mode.txt"; done
    cat /tmp/enabled.txt
    echo "PASS: enabled/disabled/default/invalid/hex outputs are byte-identical"
    cat /tmp/enabled-errors.txt
  '

#!/usr/bin/env bash
# The disc reader after the console slept: a descriptor taken away is reopened and the read repeated (music streams survive).
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
      tests/dvd_resume.cpp platform/src/os/os_alarm.cpp -lpthread -o /tmp/dvd_resume
    /tmp/dvd_resume on
    /tmp/dvd_resume off
  '

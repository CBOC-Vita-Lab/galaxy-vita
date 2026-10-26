#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 --user 65534:65534 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra \
      -fsanitize=undefined -fsanitize-trap=undefined -fms-extensions \
      -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
      -include platform/include/port/compat.h -Wno-ignored-qualifiers \
      -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function \
      -Wno-register -Wno-deprecated-enum-enum-conversion -Wno-macro-redefined -Wno-mismatched-tags -Wno-sign-compare -Wno-frame-address \
      -Iplatform/include -Idecomp/libs/RVL_SDK/include -Idecomp/include -Idecomp/libs/JSystem/include \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      tests/boot_data.cpp decomp/src/Game/AudioLib/AudParams.cpp -lpthread -o /tmp/boot_data
    GXV_BOOTDATA=1 timeout 10s /tmp/boot_data enabled
    GXV_BOOTDATA=0 /tmp/boot_data disabled
    env -u GXV_BOOTDATA /tmp/boot_data disabled
    GXV_BOOTDATA=invalid /tmp/boot_data disabled
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -fsanitize=undefined -fsanitize-trap=undefined \
      -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -include platform/include/port/compat.h \
      -Iplatform/include -Idecomp/libs/RVL_SDK/include tests/boot_data_startup.cpp -o /tmp/boot_data_startup
    /tmp/boot_data_startup
  '

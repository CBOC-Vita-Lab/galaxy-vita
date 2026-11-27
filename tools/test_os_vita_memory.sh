#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    for mode in native desktop; do
    define=()
    if test "$mode" = native; then define=(-D__vita__); fi
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra \
      -fms-extensions -DTARGET_PC=1 -DNDEBUG=1 -DPORT_PLATFORM_CODE=1 "${define[@]}" \
      -include platform/include/port/compat.h -Wno-ignored-qualifiers \
      -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas \
      -Wno-unused-function -Wno-register -Wno-bitwise-op-parentheses \
      -Iplatform/include -Ivita/runtime -Idecomp/libs/RVL_SDK/include \
      -Idecomp/libs/JSystem/include -Idecomp/libs/nw4r/include -Idecomp/include \
      -Wno-register -Wno-c++11-narrowing -Wno-format -Wno-unused-parameter \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -mcmodel=large -fno-pie -no-pie -fuse-ld=lld -Wl,--image-base=0xB8000000 \
      tests/os_vita_memory.cpp tests/os_vita_heaps.cpp tests/dvd_heap_routing.cpp platform/src/os/os_mem.cpp \
      decomp/src/Game/System/HeapMemoryWatcher.cpp decomp/src/Game/System/Overwrite.cpp \
      decomp/src/Game/Util/MemoryUtil.cpp \
      decomp/src/JSystem/JKernel/JKRHeap.cpp decomp/src/JSystem/JKernel/JKRExpHeap.cpp \
      decomp/src/JSystem/JKernel/JKRSolidHeap.cpp decomp/src/JSystem/JKernel/JKRDisposer.cpp \
      decomp/src/JSystem/JAudio2/JASHeapCtrl.cpp \
      decomp/src/JSystem/JSupport/JSUList.cpp -lpthread -ldl -o "/tmp/os_heaps_$mode"
    if test "$mode" = native; then timeout 10s "/tmp/os_heaps_$mode"; fi
    PETARI_NATIVE_ADDRESSES=1 timeout 10s "/tmp/os_heaps_$mode" heaps
    done
  '

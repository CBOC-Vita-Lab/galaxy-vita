#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
  clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra \
    -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer -fms-extensions \
    -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
    -include platform/include/port/compat.h -Wno-ignored-qualifiers \
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function -Wno-frame-address \
    -Iplatform/include -Idecomp/libs/RVL_SDK/include \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tests/dvd_priority.cpp platform/src/os/os_alarm.cpp -lpthread -l:libasan.so.8 -l:libubsan.so.1 -o /tmp/dvd_priority
  export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  failed=0
  for name in baseline order cancel sync active callback invalid pressure; do
    set +e
    timeout 15s /tmp/dvd_priority "$name" > "/tmp/dvd-priority-$name.log" 2>&1
    status=$?
    set -e
    cat "/tmp/dvd-priority-$name.log"
    printf "case=%s status=%s\n" "$name" "$status"
    if test "$status" != 0; then failed=$((failed+1)); fi
  done
  printf "priority failing_cases=%s\n" "$failed"
  test "$failed" = 0

  # The real low-address JKR heap fixture is separate from sanitizer shadow memory.
  clang++ -std=c++20 -stdlib=libc++ -O2 -g -Wall -Wextra \
    -fms-extensions -DTARGET_PC=1 -DNDEBUG=1 -DPORT_PLATFORM_CODE=1 \
    -DPORT_RUNTIME_DIAGNOSTICS=0 -DDVD_HEAP_FIXTURE=1 \
    -include platform/include/port/compat.h -Wno-ignored-qualifiers \
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas \
    -Wno-unused-function -Wno-register -Wno-bitwise-op-parentheses -Wno-frame-address \
    -Iplatform/include -Ivita/runtime -Idecomp/libs/RVL_SDK/include \
    -Idecomp/libs/JSystem/include -Idecomp/libs/nw4r/include -Idecomp/include \
    -Wno-c++11-narrowing -Wno-format -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -mcmodel=large -fno-pie -no-pie -fuse-ld=lld -Wl,--image-base=0xB8000000 \
    tests/os_vita_memory.cpp tests/os_vita_heaps.cpp tests/dvd_priority.cpp platform/src/os/os_mem.cpp \
    decomp/src/Game/System/HeapMemoryWatcher.cpp decomp/src/Game/System/Overwrite.cpp \
    decomp/src/Game/Util/MemoryUtil.cpp \
    decomp/src/JSystem/JKernel/JKRHeap.cpp decomp/src/JSystem/JKernel/JKRExpHeap.cpp \
    decomp/src/JSystem/JKernel/JKRSolidHeap.cpp decomp/src/JSystem/JKernel/JKRDisposer.cpp \
    decomp/src/JSystem/JAudio2/JASHeapCtrl.cpp \
    decomp/src/JSystem/JSupport/JSUList.cpp -lpthread -ldl -o /tmp/dvd_priority_heap
  PETARI_NATIVE_ADDRESSES=1 timeout 30s /tmp/dvd_priority_heap heaps
  '

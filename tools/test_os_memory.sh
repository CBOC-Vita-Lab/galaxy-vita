#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=codex-galaxy-native:20261007-arm64
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  -v "$root:/work:ro" -w /work "$image" bash -euo pipefail -c '
    cflags=(-std=c11 -O2 -fms-extensions -ffunction-sections -fdata-sections -DTARGET_PC=1 -DNDEBUG=1
      -include platform/desktop/c_compat.h -include platform/include/port/compat.h
      -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-header-guard -Wno-switch
      -Wno-implicit-function-declaration -Wno-c23-extensions
      -Iplatform/include -Idecomp/libs/RVL_SDK/include)
    for unit in GXTexture GXMisc; do
      clang "${cflags[@]}" -c "decomp/src/RVL_SDK/gx/$unit.c" -o "/tmp/$unit.o"
    done
    for source in decomp/src/Game/Map/KCollision.cpp decomp/src/Game/Util/JMapInfo.cpp; do
      clang++ -std=c++20 -stdlib=libc++ -O2 -fms-extensions -DTARGET_PC=1 -DNDEBUG=1 \
        -include platform/include/port/compat.h -Wno-register -Wno-int-to-pointer-cast \
        -Wno-pointer-to-int-cast -Wno-c++11-narrowing -Wno-unknown-pragmas \
        -Iplatform/include -Idecomp/libs/RVL_SDK/include \
        -isystem decomp/libs/JSystem/include -isystem decomp/include \
        -ffunction-sections -fdata-sections -c "$source" -o "/tmp/$(basename "$source").o"
    done
    clang++ -std=c++20 -stdlib=libc++ -O2 -Wall -Wextra -Werror \
      -fms-extensions -DTARGET_PC=1 -DNDEBUG=1 -DPORT_PLATFORM_CODE=1 \
      -include platform/include/port/compat.h -Wno-ignored-qualifiers \
      -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function \
      -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -isystem decomp/libs/JSystem/include -isystem decomp/include \
      -Wno-macro-redefined -Wno-bitwise-op-parentheses -Wno-register \
      -ffunction-sections -fdata-sections -Wno-sign-compare -Wno-deprecated-enum-enum-conversion \
      tests/os_memory.cpp tests/j3d_array_producer.cpp platform/src/os/os_mem.cpp decomp/src/JSystem/JUtility/JUTTexture.cpp \
      -x c++ decomp/src/RVL_SDK/aralt/aralt.c \
      -x none /tmp/GXTexture.o /tmp/GXMisc.o /tmp/KCollision.cpp.o /tmp/JMapInfo.cpp.o -Wl,--gc-sections \
      -mcmodel=large -fno-pie -no-pie -fuse-ld=lld -Wl,--image-base=0xB8000000 \
      -o /tmp/os_memory -ldl -lpthread
    GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 PETARI_NATIVE_ADDRESSES=1 /tmp/os_memory
    GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 /tmp/os_memory
    for case in physical-hole masked-physical native-hole bad-alignment arena-exhausted arena-outside low-overflow low-guard resource-high-base; do
      if GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 PETARI_NATIVE_ADDRESSES=1 /tmp/os_memory "$case"; then
        echo "Invalid request unexpectedly passed: $case" >&2
        exit 1
      else
        test "$?" = 2
      fi
    done
  '

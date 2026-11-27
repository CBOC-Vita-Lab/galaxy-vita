#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -fno-exceptions -fno-rtti \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -DPORT_PLATFORM_CODE=1 \
      -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions \
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-microsoft-cast \
      -Wno-multichar -Wno-invalid-source-encoding -Wno-deprecated-volatile \
      -Wno-register -Wno-macro-redefined -Wno-self-assign-field -Wno-array-bounds \
      -Wno-deprecated-enum-enum-conversion \
      -ffunction-sections -fdata-sections -include platform/include/port/compat.h \
      -Iplatform/include -Idecomp/include -Idecomp/libs/JSystem/include \
      -Idecomp/libs/RVL_SDK/include -Idecomp/libs/nw4r/include -Idecomp/libs/RVLFaceLib/include \
      tests/jai_stream_prepare.cpp decomp/src/JSystem/JAudio2/JAIStream.cpp \
      decomp/src/JSystem/JAudio2/JAIStreamMgr.cpp decomp/src/JSystem/JAudio2/JAIStreamDataMgr.cpp \
      decomp/src/JSystem/JAudio2/JASAramStream.cpp decomp/src/JSystem/JAudio2/JAISound.cpp \
      decomp/src/JSystem/JAudio2/JAISoundChild.cpp \
      decomp/src/JSystem/JAudio2/JAISoundParams.cpp decomp/src/JSystem/JAudio2/JASSoundParams.cpp \
      decomp/src/JSystem/JSupport/JSUList.cpp -fuse-ld=lld -Wl,--gc-sections \
      -Wl,--wrap=_ZN13JASAramStream12channelStartEv -Wl,--wrap=_ZN13JASAramStream11channelStopEt \
      -o /tmp/jai_stream_prepare -l:libasan.so.8 -l:libubsan.so.1
    timeout 20s /tmp/jai_stream_prepare
  '

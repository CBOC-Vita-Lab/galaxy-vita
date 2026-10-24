#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
fixture=$(mktemp -d "$root/local/frame-sink-order.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
node "$root/tools/prepare_vgx_frame_sink_fixture.cjs" "$fixture" "${FRAME_SINK_MUTANT:-none}"
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" \
    --mount "type=bind,source=$fixture,target=/fixture,readonly" \
    --mount "type=bind,source=$sdk,target=/sdk,readonly" -w /work \
    -e "FRAME_SINK_TSAN=${FRAME_SINK_TSAN:-0}" \
    codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
        export ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
        sanitizer=(-fsanitize=address,undefined -l:libasan.so.8 -l:libubsan.so.1)
        if test "$FRAME_SINK_TSAN" = 1; then sanitizer=(-fsanitize=thread -l:libtsan.so.2); fi
        common=(-std=c++20 -stdlib=libc++ -O1 -g -D__vita__ -DTARGET_PC=1 -DNDEBUG=1 -fms-extensions
            -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -DPORT_LOGGING=0 -DPORT_PLATFORM_CODE=1
            -ffunction-sections -fdata-sections -Wl,--gc-sections
            -I/fixture -Iplatform/include -Ivita/runtime -Iplatform/src/gx -Iplatform/vgx/include -Iplatform/vgx/src
            -Iplatform/vgx/third_party -Idecomp/libs/RVL_SDK/include -idirafter /sdk/arm-vita-eabi/include
            -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas
            -Wno-deprecated-declarations -fno-sanitize-link-runtime -fno-omit-frame-pointer)
        for diagnostics in 0 1; do
            clang++ "${common[@]}" -DPORT_RUNTIME_DIAGNOSTICS=$diagnostics tests/vgx_frame_sink_order.cpp \
                platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp platform/vgx/src/vgx_dlcache.cpp \
                platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp \
                platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp \
                platform/vgx/src/vgx_backend_null.cpp -o /tmp/frame-sink-order "${sanitizer[@]}"
            timeout 30s /tmp/frame-sink-order
        done
    '

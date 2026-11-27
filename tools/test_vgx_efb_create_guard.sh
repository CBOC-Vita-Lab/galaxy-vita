#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --name "galaxy-efb-create-$$" --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  -e "CASES=${CASES:-framebuffer texture healthy read-once startup-framebuffer startup-texture}" -e "GUARDS=${GUARDS:-unset 0 1}" \
  -e "KEEP_RTS=${KEEP_RTS:-0 1}" \
  -e "EXPECT_GUARD=${EXPECT_GUARD:-}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Wl,--wrap=glGenFramebuffers,--wrap=glGenTextures,--wrap=glClear \
      -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vgx_efb_create_guard.cpp platform/vgx/src/vgx_shadergen.cpp \
      -o /tmp/efb-create -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
    export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for keep in $KEEP_RTS; do
    for guard in $GUARDS; do
      expected=0; if [[ $guard == 1 ]]; then expected=1; fi
      expected=${EXPECT_GUARD:-$expected}
      for mode in $CASES; do
        if [[ $guard == unset ]]; then setting=(env -u GXV_EFB_CREATE_GUARD); else setting=(env GXV_EFB_CREATE_GUARD=$guard); fi
        printf "keep_rt=%s " "$keep"
        "${setting[@]}" GXV_KEEP_RT=$keep timeout 30 /tmp/efb-create "$mode" "$expected"
      done
    done
    done'

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 -e GALLIUM_DRIVER=softpipe \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    for probe in 0 1; do
      clang++ -std=c++17 -stdlib=libc++ -O1 -g -DVGX_NO_NEON -D__VGX_VITA__ -DVGX_COLDTRACE=$probe -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/vgx/src -Iplatform/vgx/third_party \
        -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
        tests/vgx_coldtrace_backend.cpp platform/vgx/src/vgx_shadergen.cpp -o /tmp/coldtrace-backend \
        -Wl,--wrap=glCompileShader -Wl,--wrap=glLinkProgram -Wl,--wrap=glDrawElements \
        -Wl,--wrap=glGenerateMipmap -Wl,--wrap=glBufferData -lEGL -lGLESv2 -pthread -l:libasan.so.8 -l:libubsan.so.1
      export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
      for mode in compile read link draw upload stream present direct idle freeze-present; do
        timeout 30s /tmp/coldtrace-backend "$mode" > "/tmp/coldtrace-backend-$probe-$mode.json"
        cat "/tmp/coldtrace-backend-$probe-$mode.json"
      done
      if test "$probe" = 1; then
        for mode in compile read link draw upload stream present direct idle freeze-present; do
          timeout 30s /tmp/coldtrace-backend "$mode" report > "/tmp/coldtrace-backend-$mode-report.json"
          python3 tools/analyze_vgx_coldtrace.py "/tmp/coldtrace-backend-$mode-report.json" --json > "/tmp/coldtrace-backend-$mode-analysis.json"
        done
      fi
    done
    python3 -c '"'"'import json; from pathlib import Path
for mode in "compile read link draw upload stream present direct idle freeze-present".split():
 a=json.loads(Path(f"/tmp/coldtrace-backend-0-{mode}.json").read_text()); b=json.loads(Path(f"/tmp/coldtrace-backend-1-{mode}.json").read_text()); a.pop("probe"); b.pop("probe"); assert a==b,(mode,a,b)
print("PASS:20production Mesa attribution scenarios; exact cross-probe pixel and pre-reset GL-call parity;10real report round trips; not Vita FPS")'"'"'
  '

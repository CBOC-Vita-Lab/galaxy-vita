#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  -e "CSV_TSAN=${CSV_TSAN:-0}" \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    sanitize=(-fsanitize=address,undefined -l:libasan.so.8 -l:libubsan.so.1)
    if test "$CSV_TSAN" = 1; then sanitize=(-fsanitize=thread -l:libtsan.so.2); fi
    for target in host vita; do
    flags=()
    if test "$target" = vita; then flags=(-D__VGX_VITA__); fi
    clang++ -std=c++17 -stdlib=libc++ -O1 -g -pthread \
      "${flags[@]}" "${sanitize[@]}" -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      -Wl,--wrap=malloc,--wrap=open,--wrap=write,--wrap=close,--wrap=unlink \
      tests/vgx_diagnostic_csv.cpp -o /tmp/diagnostic-csv
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 /tmp/diagnostic-csv
    done
  '

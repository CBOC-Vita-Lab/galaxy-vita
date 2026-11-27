#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    adapter=()
    if test -f vita/runtime/flat_view.cpp; then adapter=(vita/runtime/flat_view.cpp); fi
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -Wall -Wextra -Werror -Iplatform/include \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      tests/vita_flat_view.cpp "${adapter[@]}" -o /tmp/vita_flat_view -l:libasan.so.8 -l:libubsan.so.1
    /tmp/vita_flat_view
  '

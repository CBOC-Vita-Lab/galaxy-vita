#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
production=$root
if [[ $# == 2 && $1 == --production-root ]]; then
  production=$(cd "$2" && pwd)
elif [[ $# != 0 ]]; then
  printf 'Usage: %s [--production-root checkout-with-WP-T]\n' "$0" >&2
  exit 2
fi
# WP-R follow-up depends on WP-T. A second read-only source root permits its
# validation before integration without modifying either isolated worktree.
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$production,target=/production,readonly" -w /production \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
    clang++ -std=c++20 -stdlib=libc++ -O1 -g -DTARGET_PC=1 -DNDEBUG=1 \
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_PLATFORM_CODE=1 -fms-extensions \
      -DPORT_TEST_OS_SOURCE=\"/production/platform/src/os/os_thread.cpp\" \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -Iplatform/include -Idecomp/libs/RVL_SDK/include \
      -include platform/include/port/compat.h -Wno-register -Wno-unknown-pragmas -Wno-deprecated-volatile \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      /work/tests/boot_data_park.cpp platform/src/dvd/dvd.cpp -pthread \
      -o /tmp/boot_data_park -l:libasan.so.8 -l:libubsan.so.1
    timeout 10s /tmp/boot_data_park
  '

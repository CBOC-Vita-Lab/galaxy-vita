#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${VITASDK:?Set VITASDK to existing headers for native-adapter host tests}"
modes=(off cold cold-playstation stale allocation-failure present-failure restart-failure running running-prior running-failure running-scanout-failure running-restore-set-failure running-restore-wait-failure running-restart running-font-oom font-flat font-bucket font-cased font-missing font-corrupt font-short-read font-eof font-read-error font-close-error font-ambiguous font-oversized font-bounded font-bucket-limit font-directory-read-error font-directory-close-error)
if (($#)); then modes=("$@"); fi
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
  --mount "type=bind,source=$root,target=/work,readonly" \
  --mount "type=bind,source=$VITASDK,target=/sdk,readonly" -w /work \
  codex-galaxy-native:20261007-arm64 bash -euo pipefail -c '
    clang++ -std=c++20 -stdlib=libc++ -fno-exceptions -fno-rtti -O1 -g -pthread \
      -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer \
      -Iplatform/include -Iplatform/vgx/src -Ivita/runtime -idirafter /sdk/arm-vita-eabi/include \
      tests/settings_runtime.cpp platform/src/settings/{player_settings,settings_store,settings_launch,settings_font}.cpp \
      platform/src/input/control_bindings.cpp -o /tmp/settings-runtime -l:libasan.so.8 -l:libubsan.so.1
    for mode in "$@"; do timeout 20 /tmp/settings-runtime "$mode"; done
  ' test-settings-runtime "${modes[@]}"

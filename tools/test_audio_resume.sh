#!/usr/bin/env bash
# Vita audio after the console slept: reopen on failure, retry schedule for refused opens, resume reports, bounded close.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-local/audio-resume}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iplatform/include -Itests/settings_stubs tests/audio_close.cpp -o "$out/close"
"$out/close"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 \
  -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 -DPORT_ARMHF_HARNESS -include platform/include/port/compat.h \
  -Wno-ignored-qualifiers -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function \
  -Wno-frame-address -Iplatform/include -Idecomp/libs/RVL_SDK/include tests/audio_reopen.cpp -pthread -o "$out/reopen"
for mode in retry single resume; do "$out/reopen" "$mode"; done

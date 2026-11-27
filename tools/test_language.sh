#!/usr/bin/env bash
# Host tests of the language choice: disc and language selection (tests/dvd_language.cpp: US / European / Japanese copies, setting, console
# language, English fallback), the Vita system-language mapping (tests/system_language.cpp) and the multiplayer texts (tests/mp/l10n_test.cpp).
# Needs only clang++/g++ on the host (no Docker, no game data). Usage: bash tools/test_language.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
nice clang++ -std=c++20 -O1 -g -Wall -Wextra -fms-extensions -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
  -Wno-ignored-qualifiers -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-pragmas -Wno-unused-function -Wno-frame-address -Wno-unused-parameter \
  -Dport_vsnprintf=vsnprintf -Iplatform/include -Idecomp/libs/RVL_SDK/include tests/dvd_language.cpp -lpthread -Wl,--unresolved-symbols=ignore-all -static -o "$out/dvd_language" 2>/dev/null
"$out/dvd_language"
nice clang++ -std=c++20 -Iplatform/include tests/system_language.cpp -o "$out/system_language"
"$out/system_language"
nice g++ -std=c++20 -fshort-wchar -Idecomp/include -Iplatform/include tests/mp/l10n_test.cpp decomp/src/Game/MP/MpMessages.cpp decomp/src/Game/MP/MpMessagesExtra.cpp -o "$out/l10n"
"$out/l10n" 2>&1 | tail -1

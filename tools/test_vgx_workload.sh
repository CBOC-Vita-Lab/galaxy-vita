#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${NODE:-node}" "$root/tools/prepare_vgx_view_entry_fixture.cjs" "$root/decomp/src/Game/Scene/GameScene.cpp" "$out/vgx_view_entry_fixture.inc"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/src" "$root/tests/vgx_workload_test.cpp" -pthread -o "$out/workload"
for mode in off timing invalid concurrent reporting backend view; do
  env -u GXV_WORKLOAD_PROBE -u GXV_WORKLOAD_STRIDE "$out/workload" "$mode"
done
for pair in "0 1" "1 1" "32 32" "1024 1024" "999999 1024"; do
  read -r setting expected <<< "$pair"
  GXV_WORKLOAD_STRIDE=$setting "$out/workload" sampling "$expected"
done
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -DPORT_LOGGING=0 \
  -I"$root/platform/vgx/src" "$root/tests/vgx_workload_test.cpp" -pthread -o "$out/silent-workload"
GXV_WORKLOAD_PROBE=1 "$out/silent-workload" off
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -D__vita__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" -I"$out" \
  "$root/tests/vgx_workload_hooks.cpp" -pthread -o "$out/workload-hooks"
env -u GXV_WORKLOAD_PROBE -u GXV_PHASECPU "$out/workload-hooks"
env -u GXV_WORKLOAD_PROBE -u GXV_PHASECPU "$out/workload-hooks" on
for mode in nonvita vita-silent; do
  defines=(-DPORT_LOGGING=1)
  if test "$mode" = vita-silent; then defines=(-DPORT_LOGGING=0 -D__vita__); fi
  "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0 \
    "${defines[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/platform/include" -I"$out" "$root/tests/vgx_workload_hooks.cpp" -pthread -o "$out/workload-hooks-$mode"
  env -u GXV_WORKLOAD_PROBE -u GXV_PHASECPU "$out/workload-hooks-$mode" on inert
  nm "$out/workload-hooks-$mode" > "$out/$mode-symbols.txt"
  if grep -q port_workload_view_entry "$out/$mode-symbols.txt"; then
    echo 'FAIL: view wrapper exports must be absent outside logging-enabled Vita' >&2; exit 1
  fi
done
if test "$(uname -s)" = Linux; then
  "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -D__vita__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
    -DWORKLOAD_TEST_EMUTLS=1 -femulated-tls -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/platform/include" -I"$out" "$root/tests/vgx_workload_hooks.cpp" \
    -Wl,--wrap=__emutls_get_address -pthread -o "$out/workload-hooks-emutls"
  env -u GXV_WORKLOAD_PROBE -u GXV_PHASECPU "$out/workload-hooks-emutls"
  env -u GXV_WORKLOAD_PROBE -u GXV_PHASECPU "$out/workload-hooks-emutls" on
fi

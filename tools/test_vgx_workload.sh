#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/src" "$root/tests/vgx_workload_test.cpp" -pthread -o "$out/workload"
for mode in off timing invalid concurrent reporting; do
  env -u GXV_WORKLOAD_PROBE -u GXV_WORKLOAD_STRIDE "$out/workload" "$mode"
done
for pair in "0 1" "1 1" "32 32" "1024 1024" "999999 1024"; do
  read -r setting expected <<< "$pair"
  GXV_WORKLOAD_STRIDE=$setting "$out/workload" sampling "$expected"
done
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -DPORT_LOGGING=0 \
  -I"$root/platform/vgx/src" "$root/tests/vgx_workload_test.cpp" -pthread -o "$out/silent-workload"
GXV_WORKLOAD_PROBE=1 "$out/silent-workload" off
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -D__vita__ -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" \
  "$root/tests/vgx_workload_hooks.cpp" -pthread -o "$out/workload-hooks"
env -u GXV_WORKLOAD_PROBE "$out/workload-hooks"
env -u GXV_WORKLOAD_PROBE "$out/workload-hooks" on

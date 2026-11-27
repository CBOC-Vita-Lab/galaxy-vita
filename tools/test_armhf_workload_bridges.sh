#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_ARMHF_HARNESS=1
  -DPORT_RUNTIME_DIAGNOSTICS=0 -fms-extensions -fno-strict-aliasing -fno-fast-math
  -Wno-register -Wno-c++11-narrowing -Wno-format -fsanitize=address,undefined -fno-omit-frame-pointer
  -I"$root/platform/include" -I"$root/platform/src/port" -I"$root/decomp/include"
  -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/JSystem/include" -pthread)
for mode in contract absent zero silent; do
  defs=(-DPORT_LOGGING=1)
  case "$mode" in
    contract) defs+=(-DPORT_WORKLOAD_GAME_BRIDGES=1);;
    zero) defs+=(-DPORT_WORKLOAD_GAME_BRIDGES=0);;
    silent) defs=(-DPORT_LOGGING=0 -DPORT_WORKLOAD_GAME_BRIDGES=1);;
  esac
  if test "$mode" = contract; then
    "${CXX:-clang++}" "${flags[@]}" "${defs[@]}" "$root/tests/armhf_workload_bridges.cpp" -o "$out/$mode"
    env -u GXV_WORKLOAD_PROBE -u GXV_ANIM_CATEGORIES -u GXV_ANIM_FAMILIES -u GXV_ANIM_MATERIAL "$out/$mode" off
    GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_MATERIAL=1 GXV_ANIM_STRIDE=1 "$out/$mode" on
  else
    "${CXX:-clang++}" "${flags[@]}" "${defs[@]}" -c "$root/platform/src/port/perf.cpp" -o "$out/$mode.o"
    nm "$out/$mode.o" > "$out/$mode-symbols.txt"
    if grep -q port_workload_ "$out/$mode-symbols.txt"; then
      echo "FAIL: $mode must not export workload game bridges" >&2; exit 1
    fi
    if ! grep -q port_thread_cpu_us "$out/$mode-symbols.txt"; then
      echo "FAIL: $mode must retain the Linux CPU clock" >&2; exit 1
    fi
  fi
done
if test "$(uname -s)" = Linux && "${CXX:-clang++}" --version | head -1 | grep -qi clang; then
  "${CXX:-clang++}" "${flags[@]}" -DPORT_LOGGING=1 -DPORT_WORKLOAD_GAME_BRIDGES=1 \
    -DBRIDGE_TEST_EMUTLS=1 -femulated-tls "$root/tests/armhf_workload_bridges.cpp" \
    -Wl,--wrap=__emutls_get_address -o "$out/emutls"
  env -u GXV_WORKLOAD_PROBE -u GXV_ANIM_CATEGORIES -u GXV_ANIM_FAMILIES -u GXV_ANIM_MATERIAL "$out/emutls" off
fi

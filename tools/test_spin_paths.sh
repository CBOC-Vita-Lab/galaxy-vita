#!/usr/bin/env bash
# Square/R spin reaches the game as a shake in play, menus, touch, tilt, zoom menu and after Settings; shake latch (GXV_SHAKE_LATCH).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk vitasdk.h; do
  [ ! -e "$sdk/arm-vita-eabi/include/$d" ] || ln -s "$sdk/arm-vita-eabi/include/$d" "$out/inc/$d"
done
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-short-enums \
  -DPORT_RUNTIME_DIAGNOSTICS=0 -DTARGET_PC -include "$root/tests/glibc_fabs_shim.h" -isystem "$out/inc" \
  -I"$root/platform/include" -I"$root/vita/runtime" -I"$root/decomp/libs/RVL_SDK/include" \
  "$root/tests/spin_paths.cpp" "$root/vita/runtime/input.cpp" \
  "$root/platform/src/input/control_bindings.cpp" "$root/platform/src/input/wpad.cpp" -o "$out/test"
for remap in 0 1; do for latch in 0 1; do "$out/test" "$remap" "$latch"; done; done

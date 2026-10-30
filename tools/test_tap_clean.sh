#!/usr/bin/env bash
# A finger resting on the screen during a dialogue: held A (old) versus one clean press (GXV_TAP_CLEAN=1).
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
  "$root/tests/tap_clean.cpp" "$root/vita/runtime/input.cpp" \
  "$root/platform/src/input/control_bindings.cpp" "$root/platform/src/input/wpad.cpp" -o "$out/test"
"$out/test" off
"$out/test" on

#!/usr/bin/env bash
# One press of Square (or R) gives exactly one spin, with the game's own swing detector (WPadHVSwing, WPadAcceleration) run on
# what the Vita input layer and KPADRead hand it: taps of 1-10 updates at 60 and 30 updates a second, after A, under water.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk vitasdk.h; do
  [ ! -e "$sdk/arm-vita-eabi/include/$d" ] || ln -s "$sdk/arm-vita-eabi/include/$d" "$out/inc/$d"
done
# The test restates MarioActor::updateControllerSwing: fail if the decompilation's text moved away from the restatement.
mario="$root/decomp/src/Game/Player/MarioActorPad.cpp"
for line in 'if (checkButtonType(3, false) || checkButtonType(4, false)) {' '_F1C = 10;' 'bool F20 = _F20;' \
            'bool isCorePadSwing = !mMario->isSwimming() ? MR::isCorePadSwing(0) : MR::isCorePadSwingTrigger(0);' \
            '} else if (isCorePadSwing && F20 == 0) {' 'if (isCorePadSwing && _946 > 10 && _F1C < 8) {' '_F1C = 8;'; do
  grep -qF -- "$line" "$mario" || { echo "decomp MarioActorPad.cpp changed: $line"; exit 1; }
done
grep -qF -- '/* mSpinIntervalTime */ 45,' "$root/decomp/src/Game/Player/MarioConst.cpp"
grep -qF -- '_946 = mConst->getTable()->mSpinIntervalTime + 0x22;' "$root/decomp/src/Game/Player/MarioActor.cpp"
flags=(-std=gnu++20 -O1 -g -DTARGET_PC=1 -DVERSION=0 -fms-extensions -fno-strict-aliasing -fwrapv -fsigned-char -fshort-wchar
  -Wno-register -Wno-multichar -Wno-invalid-offsetof -Wno-c++11-narrowing -Wno-writable-strings -Wno-unknown-pragmas -Wno-return-type
  -Wno-implicit-function-declaration -Wno-microsoft-include -Wno-language-extension-token -Wno-dangling-else
  -include "$root/platform/include/port/compat.h"
  -I"$root/tests/spin_detect_stubs" -I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/JSystem/include"
  -I"$root/decomp/libs/RVL_SDK/include")
CXX=${CXX:-clang++}
"$CXX" "${flags[@]}" -fsanitize=address,undefined -c "$root/decomp/src/Game/System/WPadAcceleration.cpp" -o "$out/acc.o"
"$CXX" "${flags[@]}" -fsanitize=address,undefined -c "$root/decomp/src/Game/System/WPadHVSwing.cpp" -o "$out/swing.o"
clang -std=gnu11 -O1 -g -DTARGET_PC=1 -fsanitize=address,undefined -include "$root/tests/glibc_fabs_shim.h" -include "$root/platform/include/port/compat.h" -Wno-implicit-function-declaration \
  -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -c "$root/platform/src/sdk/mtx.c" -o "$out/mtx.o"
"$CXX" "${flags[@]}" -fsanitize=address,undefined -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/glibc_fabs_shim.h" -isystem "$out/inc" \
  -I"$root/vita/runtime" \
  "$root/tests/spin_detect.cpp" "$root/vita/runtime/input.cpp" "$root/platform/src/input/control_bindings.cpp" \
  "$root/platform/src/input/wpad.cpp" "$out/acc.o" "$out/swing.o" "$out/mtx.o" -o "$out/test"
status=0
for remap in 0 1; do for latch in 0 1; do for pulse in ${PULSE_MODES:-0 1}; do
  "$out/test" "$remap" "$latch" "$pulse" || status=1
done; done; done
exit $status

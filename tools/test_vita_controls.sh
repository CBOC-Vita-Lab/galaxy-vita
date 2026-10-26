#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/inc"
for d in psp2 psp2common vitasdk vitasdk.h; do
  [ ! -e "$sdk/arm-vita-eabi/include/$d" ] || ln -s "$sdk/arm-vita-eabi/include/$d" "$out/inc/$d"
done
git -C "$root" show 392ee7009fe1152b5bd397e6398f00404303a264:vita/runtime/input.cpp > "$out/oracle.cpp"
flags=(-std=c++20 -O1 -g -fsanitize=address,undefined -fno-short-enums -isystem "$out/inc" -I"$root/platform/include" -I"$root/vita/runtime")
clang++ "${flags[@]}" -Dport_vita_input_init=oracle_init -Dport_vita_input_poll=oracle_poll \
  -Dport_vita_input_shutdown=oracle_shutdown -Dport_vita_input_recenter=oracle_recenter -c "$out/oracle.cpp" -o "$out/oracle.o"
clang++ "${flags[@]}" "$root/tests/vita_controls.cpp" "$root/vita/runtime/input.cpp" \
  "$root/platform/src/input/control_bindings.cpp" "$out/oracle.o" -o "$out/test"
"$out/test" off
GXV_SETTINGS=1 "$out/test" off
"$out/test" default
"$out/test" on
"$out/test" calibration
"$out/test" escape

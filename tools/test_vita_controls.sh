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
# 1.2 external pad layout (GAL-032) and mouse-style aiming (GAL-031)
GXV_PAD_DEFAULTS=1 "$out/test" off
GXV_PAD_DEFAULTS=1 "$out/test" default
GXV_PAD_DEFAULTS=1 "$out/test" on
GXV_PAD_DEFAULTS=1 "$out/test" pad
GXV_PAD_DEFAULTS=1 "$out/test" padhandheld
"$out/test" padlegacy
GXV_MOUSE_AIM=1 "$out/test" mouse
GXV_MOUSE_AIM=1 GXV_PAD_DEFAULTS=1 "$out/test" mousepad
# Tilt sensitivity (GXV_TILT_GAIN) and Tilt with (GXV_TILT_STICK): the sensor path, the left stick on the handheld, and a pad without a sensor
"$out/test" tilt 100 sensor
for p in 50 63 75 88 120 140 160 180 200; do
  GXV_TILT_GAIN=$p "$out/test" tilt $p sensor
  GXV_TILT_GAIN=$p GXV_TILT_STICK=1 "$out/test" tilt $p stick
  GXV_TILT_GAIN=$p GXV_PAD_DEFAULTS=1 "$out/test" tiltpad $p
done
GXV_TILT_STICK=1 "$out/test" tilt 100 stick
GXV_PAD_DEFAULTS=1 "$out/test" tiltpad 100

#!/usr/bin/env bash
# Run inside the existing offline ARM profiling image; no Vita SDK/build.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?provide an output directory}
mkdir -p "$out/baseline-source"
out=$(cd "$out" && pwd)
cd "$root"
base=392ee7009fe1152b5bd397e6398f00404303a264
for name in vgx_state.cpp vgx_state.h; do
  if [[ -n ${POSITION_PRISTINE_DIR:-} ]]; then
    cp "$POSITION_PRISTINE_DIR/$name" "$out/baseline-source/$name"
  else
    git show "$base:platform/vgx/src/$name" > "$out/baseline-source/$name"
  fi
done
printf '%s  %s\n' \
  384b6c9db1411c1ea32ba233bf08ad374b078f1cfd4418cf02b22161568a1158 "$out/baseline-source/vgx_state.cpp" \
  6ecfe15ace1f3625a269a0893cf9328bfa036c25ffb255a3646074234f1f749a "$out/baseline-source/vgx_state.h" | sha256sum -c -
flags=(--target=arm-linux-gnueabihf --gcc-toolchain=/usr -mcpu=cortex-a9 -mfpu=neon
       -mfloat-abi=hard -mthumb -munaligned-access -std=c++17 -O2 -DNDEBUG
       -fno-fast-math -ffp-contract=on -fno-strict-aliasing -ffunction-sections -fdata-sections
       -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0
       -Iplatform/vgx/src -Iplatform/vgx/third_party)
clang++ --version
qemu-arm --version
printf 'flags:'; printf ' %s' "${flags[@]}"; printf '\n'
clang --target=arm-linux-gnueabihf --gcc-toolchain=/usr -mcpu=cortex-a9 -mfpu=neon \
  -mfloat-abi=hard -nostdlib -static tests/vgx_position_count_calibration.S -o "$out/calibration.elf"
for variant in pristine current; do
  source=platform/vgx/src/vgx_state.cpp
  includes=()
  if [[ $variant == pristine ]]; then
    source="$out/baseline-source/vgx_state.cpp"
    includes=(-I"$out/baseline-source")
  fi
  clang++ "${includes[@]}" "${flags[@]}" tests/vgx_position_defaults_arm.cpp "$source" \
    -static -Wl,--gc-sections -o "$out/$variant.elf"
  arm-linux-gnueabihf-objdump -d "$out/$variant.elf" > "$out/$variant.disassembly"
done
python3 tools/count_vgx_position_defaults_arm.py "$out"

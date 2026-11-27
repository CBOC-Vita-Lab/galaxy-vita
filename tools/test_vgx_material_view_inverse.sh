#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
leaks=0
if [[ "$(uname -s)" == Linux ]]; then leaks=1; fi
"${NODE:-node}" "$root/tools/prepare_vgx_material_view_inverse_fixture.cjs" "$out/vgx_material_view_inverse_fixture.inc"
for pc in 1 0; do
  for diagnostics in 0 1; do
   for contract in off on; do
    for profile in 0 1; do
    printf 'material view inverse: TARGET_PC=%s diagnostics=%s fp-contract=%s profile=%s\n' "$pc" "$diagnostics" "$contract" "$profile"
    defines=(-DTEST_TARGET_PC="$pc" -DPORT_RUNTIME_DIAGNOSTICS="$diagnostics" -DTEST_MTX_PROFILE="$profile")
    if [[ "$pc" == 1 ]]; then defines+=(-DTARGET_PC=1); fi
    "${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces -Wno-sign-compare -fno-fast-math -ffp-contract="$contract" \
      -frounding-math -ftrivial-auto-var-init=pattern -fsanitize=address,undefined -fno-omit-frame-pointer \
      "${defines[@]}" -I"$out" \
      "$root/tests/vgx_material_view_inverse.cpp" -o "$out/material-view-inverse-$pc-$diagnostics-$contract-$profile"
    ASAN_OPTIONS=detect_leaks="$leaks" UBSAN_OPTIONS=halt_on_error=1 "$out/material-view-inverse-$pc-$diagnostics-$contract-$profile"
    done
   done
  done
done

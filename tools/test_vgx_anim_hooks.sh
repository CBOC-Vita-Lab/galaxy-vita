#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/vgx-anim-hooks.XXXXXX")}
mkdir -p "$out"
"${NODE:-node}" "$root/tools/prepare_vgx_anim_hooks_fixture.cjs" "$root" "$out"
for variant in vita nonvita silent vita-silent; do
    defines=(-DTARGET_PC=1 -DPORT_LOGGING=1)
    if test "$variant" = vita; then defines+=(-D__vita__); fi
    if test "$variant" = silent; then defines=(-DTARGET_PC=1 -DPORT_LOGGING=0); fi
    if test "$variant" = vita-silent; then defines=(-DTARGET_PC=1 -DPORT_LOGGING=0 -D__vita__); fi
    "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -fsanitize=address,undefined -fno-omit-frame-pointer "${defines[@]}" \
        -I"$root/platform/include" -I"$out" "$root/tests/vgx_anim_hooks.cpp" -o "$out/hooks-$variant"
    "$out/hooks-$variant"
done

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Parse the generator's Python literal, so the exercised C is the shipped recipe.
python3 - "${VGX_TIMER_GENERATOR:-$root/tools/vitagl/gpu_timers.py}" "$tmp" <<'PY'
import ast
import pathlib
import sys
tree = ast.parse(pathlib.Path(sys.argv[1]).read_text())
values = [ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
          and any(isinstance(t, ast.Name) and t.id == 'RT_GPU_C' for t in n.targets)]
assert len(values) == 1
source = values[0]
assert source.count('#include "shared.h"') == 1
source = source.replace('#include "shared.h"', '')
if 'void vglRtSetRingGuard(' in source:
    source = '#define FIXTURE_HAS_RING_GUARD\n' + source
pathlib.Path(sys.argv[2], 'rt_gpu_fixture.inc').write_text(source)
PY
"${CC:-clang}" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$tmp" "$root/tests/vgx_gpu_timer_ring.c" -o "$tmp/timer-ring"
cases=${CASES:-"0 1 47 48 255 256 300 1024"}
for n in $cases; do
    "$tmp/timer-ring" "$n" "${GUARD:-1}"
done

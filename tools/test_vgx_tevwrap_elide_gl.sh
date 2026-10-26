#!/usr/bin/env bash
# Compare actual generated GLES2 framebuffer results across all three modes.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
for mode in 0 1 2; do
  GXV_TEVWRAP_ELIDE="$mode" bash "$root/tools/test_vgx_tevwrap_gl.sh" "$out/build" > "$out/mode-$mode.log"
  sed -n '/^pixel digest /p' "$out/mode-$mode.log" > "$out/mode-$mode.pixels"
  [[ $(wc -l < "$out/mode-$mode.pixels") -eq 6 ]]
  cat "$out/mode-$mode.log"
done
cmp "$out/mode-0.pixels" "$out/mode-1.pixels"
cmp "$out/mode-0.pixels" "$out/mode-2.pixels"
echo 'PASS: exact generated-GLES2 pixel digests match baseline, optimized and verified modes'

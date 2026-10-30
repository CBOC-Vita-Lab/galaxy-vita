#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
node "$root/tools/prepare_vgx_copy_alloc_fixture.cjs" "$root" "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$root/platform/vgx/src" -I"$out" "$root/tests/vgx_copy_alloc_guard.cpp" -o "$out/copy-alloc"
for scenario in first replacement name framebuffer framebuffer_existing preexisting fill_then_copy fill_bad_framebuffer fill_bad_framebuffer_new pressure pass atlas atlas_source efb fill; do
  GXV_RT_ALLOC_GUARD=1 GXV_KEEP_RT=0 "$out/copy-alloc" "$scenario"
done
GXV_RT_ALLOC_GUARD=1 GXV_KEEP_RT=1 "$out/copy-alloc" pressure
GXV_RT_ALLOC_GUARD=0 GXV_KEEP_RT=0 "$out/copy-alloc" legacy
env -u GXV_RT_ALLOC_GUARD GXV_KEEP_RT=0 "$out/copy-alloc" legacy

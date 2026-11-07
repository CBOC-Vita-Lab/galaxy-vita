#!/usr/bin/env bash
# GXV_FS_HALF: byte identity with the switch off / 2, per-key half precision with 1 / 3, and (with 1) the soundness and the measured effect of the
# analysis on a CPU model of the generated TEV pipeline plus the generated shaders on Mesa. No game data or Vita executable needed.
# An optional second argument is a vgx_shaderlist.bin: its fragment shaders are counted and run through the model as well.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
list=${2:-}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O2 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" -I"$root/tests" \
  "$root/tests/vgx_fshalf_test.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_fshalf_test"
run() { env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_fshalf_test" ${list:+"$list"} 2>&1 | grep -v '^libunwind'; }
# unset, 0 and invalid values are the off state; 2 is the all-shaders switch (the text is untouched); 1 and 3 decide per shader
run -u GXV_FS_HALF
run GXV_FS_HALF=0
run GXV_FS_HALF=bogus
run GXV_FS_HALF=2
run GXV_FS_HALF=3
run GXV_FS_HALF=1

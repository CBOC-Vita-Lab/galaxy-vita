#!/usr/bin/env bash
# GX -> GLES2 repairs (logic ops, destination alpha with blending, indirect add-previous, switched-off texture)
# on real generated shaders and real GL blending on Mesa; no game data or Vita executable needed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_tevfix_gl.cpp" "$src/vgx_shadergen.cpp" \
  "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_tevfix_gl"
run() { # <expect-indprev> [env assignment]
  local expect=$1; shift
  env -u GXV_FIX_INDPREV "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_tevfix_gl" "$expect" 2>&1 | grep -v '^libunwind'
}
echo "== GXV_FIX_INDPREV unset (default off)"; run 0 env | tee "$out/on.txt"
echo "== GXV_FIX_INDPREV=1"; run 1 env GXV_FIX_INDPREV=1 | tail -3
echo "== GXV_FIX_INDPREV=0 (old text)"; run 0 env GXV_FIX_INDPREV=0 | tee "$out/off.txt" | grep -E "^indirect|^PASS"
# An indirect shader without add-previous must be byte-identical either way (compiled-shader cache stays valid).
a=$(grep '^source hash' "$out/on.txt"); b=$(grep '^source hash' "$out/off.txt")
[[ -n $a && $a == "$b" ]] || { echo "FAIL: indirect shader without add-previous changed text ($a vs $b)"; exit 1; }
echo "source text of indirect shaders without add-previous unchanged: $a"

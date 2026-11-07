#!/usr/bin/env bash
# Host test of vitaGL patch 0014 (the vgx draw caches: fragment-program memo, uniform-ring re-use): the helper text of a patched
# custom_shaders.c (from "---- vgx draw caches" to the end of vgl_upload_frag_unifs) is compiled into tests/vgl_drawcache_test.cpp
# against models of the uniform ring (two arenas, as UNIFORM_PINGPONG), the shader patcher and the GPU's reads.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if test "$#" != 2 || test "$1" != --vitagl-source; then
    echo 'usage: test_vgl_drawcache.sh --vitagl-source <patched vitaGL root or source directory>' >&2
    exit 2
fi
src=$2
test -f "$src/custom_shaders.c" || src=$src/source
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
python3 -I - "$src/custom_shaders.c" "$fixture/vgl_drawcache_functions.inc" <<'PYEOF'
import sys
s = open(sys.argv[1]).read()
a = s.index("// ---- vgx draw caches")
b = s.index("static inline __attribute__((always_inline)) void vgl_upload_frag_unifs(")
b = s.index("\n}\n", b) + 3
open(sys.argv[2], "w").write(s[a:b].replace("static inline __attribute__((always_inline))", "static inline"))
PYEOF
"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fpermissive -w \
    -I"$fixture" "$root/tests/vgl_drawcache_test.cpp" -o "$fixture/test"
"$fixture/test"

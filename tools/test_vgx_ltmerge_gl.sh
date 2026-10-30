#!/usr/bin/env bash
# GXV_LTMERGE: one pass over the lights for a lit channel's colour and alpha, against the two passes of the generator before it.
# Real generated GLES2 vertex shaders on Mesa (pixel oracle). No game data or Vita executable needed.
#   tools/test_vgx_ltmerge_gl.sh [output-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
src="$root/platform/vgx/src"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -I"$src" -I"$root/platform/vgx/third_party" \
  "$root/tests/vgx_ltmerge_gl.cpp" "$src/vgx_shadergen.cpp" "$root/platform/vgx/host/vgx_host_egl.cpp" -lEGL -lGLESv2 -o "$out/vgx_ltmerge_gl"
run() { env "$@" LIBGL_ALWAYS_SOFTWARE=1 "$out/vgx_ltmerge_gl" 2>&1 | grep -v '^libunwind'; }
run -u GXV_LTMERGE > "$out/off.txt"
run GXV_LTMERGE=0 > "$out/zero.txt"
run GXV_LTMERGE=bogus > "$out/bogus.txt"
run GXV_LTMERGE=1 > "$out/on.txt"
head -1 "$out/off.txt"; head -1 "$out/on.txt"
python3 - "$out" <<'PY'
import sys, re
d = sys.argv[1]
def load(n):
    lines = open(f"{d}/{n}.txt").read().splitlines()
    keys = [tuple(re.findall(r"=([0-9a-f]+)", l)) for l in lines[1:] if l.startswith("key ")]
    return lines[0], keys
off, koff = load("off")
for n in ("zero", "bogus"):
    h, k = load(n)
    assert k == koff, f"{n}: same as unset"
on, kon = load("on")
assert len(koff) == len(kon) and len(koff) > 0
# the committed digest of the sources the generator made before the switch existed (from the commit before): switch off
BASE = "2e2857fec16fc46c"
m = re.search(r"src=([0-9a-f]+)", off).group(1)
assert m == BASE, f"switch off: sources changed ({m})"
changed = sum(1 for a, b in zip(koff, kon) if a[0] != b[0])
bad = [i for i, (a, b) in enumerate(zip(koff, kon)) if a[1] != b[1]]
assert not bad, f"pixels differ for keys {bad[:10]}"
assert changed > 0, "switch on changed no source"
print(f"ok: {len(koff)} keys, {changed} with merged light passes, pixels equal for all")
PY

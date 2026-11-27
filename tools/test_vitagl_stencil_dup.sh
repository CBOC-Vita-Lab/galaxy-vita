#!/usr/bin/env bash
# A patched vitaGL tree must not mark depth+stencil dirty twice in one function: a function that uses the
# VGL_MARK_DS_DIRTY macro / mark_depth_stencil_as_dirty() must not also mark ->stencilData itself.
# Usage: test_vitagl_stencil_dup.sh <vitagl-tree> ; BREAK=1 adds a duplicate to a copy and must FAIL.
set -euo pipefail
tree=${1:?vitagl tree}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
src=$tree
[[ ${BREAK:-0} == 1 ]] && { cp -r "$src" "$tmp/s"; src=$tmp/s; python3 -I - "$src/framebuffers.c" <<'PY'
import sys,re
p=sys.argv[1]; t=open(p).read()
t=t.replace("vglFramebufferMultisample(","vglFramebufferMultisample(",1)
i=t.index("vglFramebufferMultisample("); j=t.index("{",i)
t=t[:j+1]+"\n VGL_MARK_DS_DIRTY(fb); /*BREAK*/"+t[j+1:]
open(p,"w").write(t)
PY
}
python3 -I - "$src" <<'PY'
import sys,re,glob
bad=0
for f in glob.glob(sys.argv[1]+"/*.c"):
    t=open(f,errors="replace").read()
    for m in re.finditer(r"^[A-Za-z_][^;{}]*\)\s*\{",t,re.M):
        s=m.end(); d=1; i=s
        while i<len(t) and d: d+= (t[i]=="{")-(t[i]=="}"); i+=1
        body=t[s:i]
        if re.search(r"VGL_MARK_DS_DIRTY|mark_depth_stencil_as_dirty",body) and re.search(r"stencilData\S*\s*->\s*(dirty|mark)|stencilData[^;]*dirty|mark_[a-z_]*stencil",body.replace("mark_depth_stencil_as_dirty","")):
            print("DUP in",f,m.group(0)[:60].strip()); bad+=1
sys.exit(1 if bad else 0)
PY
echo "PASS: no function marks depth/stencil twice"

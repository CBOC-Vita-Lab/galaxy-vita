#!/usr/bin/env bash
# dome31: the REAL LiveActor::calcAnim (extracted from decomp/src/Game/LiveActor/LiveActor.cpp) against stubs in which every port cut says "leave this
# pose out". A galaxy-map actor (mPortNoCut) must be posed every turn; an ordinary actor is left out (control). NOCUT_MUTANT=1 strips the exemption
# from the extracted text first: the test must then FAIL (proves it tests the fix).
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
sed -n '/^void LiveActor::calcAnim() {/,/^}/p' decomp/src/Game/LiveActor/LiveActor.cpp > "$out/calcanim.inc"
test -s "$out/calcanim.inc"
if [[ "${NOCUT_MUTANT:-0}" == 1 ]]; then
  python3 - "$out/calcanim.inc" <<'P'
import sys,re
p=sys.argv[1]; s=open(p).read()
s2=re.sub(r'if \(__builtin_expect\(mPortNoCut, 0\)\) \{.*?\n        return;\n    \}\n','',s,count=1,flags=re.S)
assert s2!=s; open(p,'w').write(s2)
P
fi
mut=0; [[ "${NOCUT_MUTANT:-0}" == 1 ]] && mut=1
"${CXX:-clang++}" -std=c++17 -DTARGET_PC=1 -DNOCUT_MUTANT=$mut -O1 -g -I"$out" tests/dome31_nocut.cpp -o "$out/t"
"$out/t"

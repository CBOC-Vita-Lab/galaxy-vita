#!/usr/bin/env bash
# GXV_J3D_NEON host proof: tests/j3d_envelope_neon.cpp against the production function body of J3DMtxBuffer.cpp.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 -I - "$root/decomp/src/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp" "$out/body.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('template <bool Reuse>\nstatic void J3DEnvelopeLoop(')
b = s.index('\n}\n#endif', a)
open(sys.argv[2], 'w').write(s[a:b+2])
PY
"${CXX:-g++}" -std=c++17 -O2 -ffp-contract=off -I"$out" -I"$root/tests" "$root/tests/j3d_envelope_neon.cpp" -o "$out/envelope"
"$out/envelope"
if [[ $# -gt 0 ]]; then
    python3 -B "$root/tools/c184_skin_census.py" "$1" --fixture "$out/topology" "${@:2}"
    "${CXX:-g++}" -std=c++17 -O2 -ffp-contract=off -DC184_SKIN_BENCH -I"$out" -I"$root/tests" "$root/tests/j3d_envelope_neon.cpp" -o "$out/bench"
    "$out/bench" "$out/topology"
fi

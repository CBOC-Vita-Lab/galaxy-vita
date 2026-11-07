#!/usr/bin/env bash
# tex11: GXV_VITA_UPLOAD_SYNC=2 and the chain reserve (tests/vgx_upload_sync.cpp) on the GL stub with a model of vitaGL's texture
# storage: no hazard in any mode, the same texture calls in every mode, and how many transfer-unit waits each needs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
  -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_upload_sync.cpp" "$root/platform/vgx/src/vgx_shadergen.cpp" \
  "$root/platform/vgx/src/vgx_autoq.cpp" -o "$out/vgx_upload_sync" -lpthread
for run in "1 0" "1 1" "2 0" "2 1"; do
  set -- $run
  cp "$out/vgx_upload_sync" "$out/us_$1_$2"
  "$out/us_$1_$2" "$1" "$2"
done
cmp "$out/us_1_0.calls" "$out/us_2_0.calls"
cmp "$out/us_1_0.calls" "$out/us_2_1.calls"
cmp "$out/us_1_0.calls" "$out/us_1_1.calls"
echo "PASS: the same texture calls in every mode"

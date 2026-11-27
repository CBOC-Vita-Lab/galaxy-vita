#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
base=8aee28db
shape=decomp/src/JSystem/J3DGraphBase/J3DShapeMtx.cpp
fish=decomp/src/Game/Map/FishGroup.cpp
extract() { sed -n "/^$1/,/^}/p"; }
git show "$base:$shape" | extract 'void J3DDifferedTexMtx::loadExecute(' | sed 's/J3DDifferedTexMtx::/BaseShape::/' > "$out/base.inc"
git show "$base:$fish" | extract 'void Fish::calcAndSetBaseMtx(' | sed 's/Fish::/BaseFish::/' >> "$out/base.inc"
sed -n '/^class DrawViewInverse {/,/^};/p' "$shape" > "$out/current.inc"
extract 'void J3DDifferedTexMtx::loadExecute(' < "$shape" >> "$out/current.inc"
extract 'void Fish::calcAndSetBaseMtx(' < "$fish" >> "$out/current.inc"
sed -n '/^    void makeMtxFrontUp(/,/^    }/p' decomp/src/Game/Util/MtxUtil.cpp > "$out/orientation.inc"
sed -n '/^void C_VECNormalize(.* {$/,/^}/p' platform/src/sdk/mtx.c > "$out/normalize.inc"
src=decomp/src/JSystem/J3DGraphBase/J3DTransform.cpp
for name in J3DGetTextureMtx J3DGetTextureMtxOld J3DGetTextureMtxMaya J3DGetTextureMtxMayaOld J3DMtxProjConcat; do
    extract "void $name(" < "$src" >> "$out/math.inc"
done
sed -n '/^#if defined(__ARM_NEON)/,/^void PSMTXConcat(/p' platform/src/sdk/mtx.c >> "$out/math.inc"
extract 'u32 C_MTXInverse(' < platform/src/sdk/mtx.c >> "$out/math.inc"
source tools/host_san.sh
san=($(host_san "${CXX:-clang++}" -fsanitize=address,undefined -fno-omit-frame-pointer))
flags=(-std=c++17 -DTARGET_PC=1 -I"$out" -fno-fast-math -ffp-contract=off -ftrivial-auto-var-init=pattern)
"${CXX:-clang++}" "${flags[@]}" -O1 -g ${san[@]+"${san[@]}"} tests/c182_cpu.cpp -o "$out/test"
"$out/test"
backend=platform/vgx/src/vgx_backend_gl.cpp
git show "$base:$backend" | extract 'bool FindProjection(' | sed 's/bool FindProjection(/bool BaselineProjection(/' > "$out/projection.inc"
extract 'bool FindProjection(' < "$backend" >> "$out/projection.inc"
"${CXX:-clang++}" -std=c++17 -I"$out" -Iplatform/vgx/src -O1 -g ${san[@]+"${san[@]}"} tests/c182_projection.cpp -o "$out/projection"
"$out/projection"
if [[ "${1:-}" == --bench ]]; then
    "${CXX:-clang++}" "${flags[@]}" -O3 tests/c182_cpu.cpp -o "$out/bench"
    "$out/bench" --bench
    "${CXX:-clang++}" -std=c++17 -I"$out" -Iplatform/vgx/src -O3 tests/c182_projection.cpp -o "$out/bench-projection"
    "$out/bench-projection" --bench
fi

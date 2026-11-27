#!/usr/bin/env bash
# Optional argument: local cooked StageData folder to measure the actual follower path.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [[ -n ${1:-} ]]; then
    python3 -B tools/c183_rail_census.py "$1" "$tmp/points"
    set -- "$tmp/points"
fi
cxx=${CXX:-clang++}
# glibc_fabs_shim.h: glibc's <math.h> declares __fabs/__fabsf, which port/ppc_intrinsics.h defines. <string> before compat.h: compat.h
# renames vswprintf for game code, and libstdc++'s basic_string.h (std::to_wstring) must be parsed before that. Both harmless elsewhere.
flags=(-std=c++20 -DTARGET_PC=1 -Wno-register -Wno-array-bounds -Wno-self-assign-field -Wno-macro-redefined -Wno-gcc-compat
       -Iplatform/include -Idecomp/include -Idecomp/libs/JSystem/include -Idecomp/libs/RVL_SDK/include
       -Idecomp/libs/nw4r/include -include tests/glibc_fabs_shim.h -include string -include port/compat.h -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)
if [[ ${RAIL_SANITIZE:-0} == 1 ]]; then
    source tools/host_san.sh
    san=($(host_san "$cxx" -fsanitize=address,undefined -fno-omit-frame-pointer))
    flags+=(${san[@]+"${san[@]}"})
fi
link=(-Wl,--gc-sections)
if [[ $(uname -s) == Darwin ]]; then link=(-Wl,-dead_strip); fi
bezier=decomp/src/Game/Map/BezierRail.cpp
rider=decomp/src/Game/LiveActor/RailRider.cpp
part=decomp/src/Game/Map/RailPart.cpp
for file in "$bezier" "$rider" "$part"; do
    git show "abf063cc:$file" > "$tmp/old-${file##*/}"
done
# Replace only the object-loading constructors with deterministic fixtures.
for base in 1 0; do
    rr=$rider; br=$bezier; pt=$part
    if [[ $base == 1 ]]; then rr=$tmp/old-RailRider.cpp; br=$tmp/old-BezierRail.cpp; pt=$tmp/old-RailPart.cpp; fi
    sed '/^RailRider::RailRider(/,/^}/d' "$rr" > "$tmp/rider.cpp"
    sed '/^BezierRail::BezierRail(/,$d' "$br" > "$tmp/bezier.cpp"
    {
        printf '#include <revolution/mtx.h>\n#define __REGISTER\n'
        sed -n '/^void C_VECNormalize(.* {$/,/^}/p; /^void PSVECNormalize(/p; /^f32 C_VECSquareMag(/p; /^f32 PSVECSquareMag(/p; /^f32 C_VECMag(/p; /^f32 PSVECMag(/p' platform/src/sdk/mtx.c
        sed -n '/^void JMAVECScaleAdd(/,/^}/p' decomp/src/JSystem/JMath/JMath.cpp
        printf '#include "Game/Util/MathUtil.hpp"\nnamespace {\n'   # (the fp40 bit helpers MathUtil.cpp's isNearZero uses)
        sed -n '/^    inline u32 fp40Bits/,/^}  \/\/ namespace/p' decomp/src/Game/Util/MathUtil.cpp
        printf 'namespace MR {\n'
        sed -n '/^    bool isNearZero(f32 x, f32 tolerance)/,/^    }/p; /^    bool isNearZero(const TVec3f\& rVec, f32 tolerance)/,/^    }/p; /^    void normalize(TVec3f\* pVec)/,/^    }/p' decomp/src/Game/Util/MathUtil.cpp
        printf '}\n'
    } > "$tmp/math.cpp"
    "$cxx" "${flags[@]}" -DBASELINE=$base -O2 "$tmp/rider.cpp" "$tmp/bezier.cpp" "$pt" "$tmp/math.cpp" tests/c183_rail.cpp "${link[@]}" -o "$tmp/test"
    "$tmp/test" "$tmp/states$base" 7 "$@"
done
cmp "$tmp/states1" "$tmp/states0"
printf 'PASS: closed/open, reverse/forward, wrap/bounds, pose, tangent and current point match byte for byte.\n'

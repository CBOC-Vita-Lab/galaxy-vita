#!/bin/bash
# zoomclip23 (GXV_ZOOM_LOGIC_VIEW / GXV_ZOOM_DRAWCULL): host check of the object clipping under the handheld zoom (tests/zoom_clip.cpp)
# with the real ClippingJudge.cpp and MR::getFovyForLogic. PASS (new rule), PASS (GXV_ZOOM_LOGIC_VIEW=0 is 2.2.1's frustum), and the
# negative control (2.2.1's frustum held to the new rule) must FAIL. ZC_BASE=<tree>: headers missing here are taken from there.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/zoom_clip_test.$$"
mkdir -p "$out"
inc=()
for r in "$root" ${ZC_BASE:+"$ZC_BASE"}; do
    inc+=(-I"$r/platform/include" -I"$r/decomp/include" -I"$r/decomp/libs/JSystem/include" -I"$r/decomp/libs/RVL_SDK/include"
          -I"$r/decomp/libs/nw4r/include" -I"$r/decomp/libs/RVLFaceLib/include")
done
# the real getFovyForLogic and isPictureZoomedIn, as they are in CameraUtil.cpp
awk '/^    f32 getFovyForLogic\(\) \{/{on=1} on{print} on&&/^    bool isPictureZoomedIn\(\) \{/{b=1} b&&/^    }$/{exit}' \
    "$root/decomp/src/Game/Util/CameraUtil.cpp" > "$out/zoom_clip_logic.inc"
grep -q "mPortFovyGame" "$out/zoom_clip_logic.inc" || { echo "could not cut getFovyForLogic out of CameraUtil.cpp"; exit 2; }
printf '#include <cfloat>\nint port_vr_cull_view(float* cameraMtx, float* tanHalfX, float* tanHalfY);\n' > "$out/prelude.h"
flags=(-std=gnu++17 -O1 -DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fms-extensions -fno-exceptions -fno-rtti
       -Wno-register -w -include "$out/prelude.h" -I"$out" "${inc[@]}")
CXX="${CXX:-clang++}"
"$CXX" "${flags[@]}" -c "$root/decomp/src/Game/LiveActor/ClippingJudge.cpp" -o "$out/judge.o" || { echo "build FAILED (ClippingJudge.cpp)"; exit 2; }
"$CXX" "${flags[@]}" -c "$root/tests/zoom_clip.cpp" -o "$out/test.o" || { echo "build FAILED (zoom_clip.cpp)"; exit 2; }
"$CXX" "$out/judge.o" "$out/test.o" -lm -o "$out/zoom_clip" || { echo "link FAILED"; exit 2; }
rc=0
"$out/zoom_clip" || rc=1
GXV_ZOOM_LOGIC_VIEW=0 ZC_MODE=old "$out/zoom_clip" || rc=1
if ZC_NEG=1 "$out/zoom_clip" > "$out/neg.txt" 2>&1; then
    echo "negative control did NOT fail: the test cannot see the 2.2.1 error"; rc=1
else
    head -1 "$out/neg.txt"; echo "(negative control failed, as it must)"
fi
rm -rf "$out"
[ $rc -eq 0 ] && echo "test_zoom_clip: PASS" || echo "test_zoom_clip: FAIL"
exit $rc

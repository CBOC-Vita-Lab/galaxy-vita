#!/usr/bin/env bash
# Syntax-only compile of the decomp units touched by the Color8 byte-order fix (GXV_FIX_COLOR8, decomp/include/Game/Util/Color.hpp),
# with the host flags of CMakeLists.txt and, when VITASDK is set, the Vita target flags of vita/CMakeLists.txt. No object files.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
units=(Effect/AutoEffectInfo Util/DrawUtil MapObj/BigBubbleDrawer Ride/SpaceCocoon LiveActor/ShadowSurfaceDrawer
       Ride/Trapeze Screen/WaterCameraFilter)
inc=(-Iplatform/include -Idecomp/include -Idecomp/libs/JSystem/include -Idecomp/libs/RVL_SDK/include -Idecomp/libs/nw4r/include
     -Idecomp/libs/RVLFaceLib/include)
common=(-DTARGET_PC=1 -DNDEBUG=1 -DVERSION=0 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -fms-extensions -ferror-limit=0
        -fno-exceptions -fno-rtti -Wno-everything)
for u in "${units[@]}"; do
  nice "${CXX:-clang++}" -std=gnu++20 "${inc[@]}" "${common[@]}" -include platform/include/port/compat.h -fsyntax-only "decomp/src/Game/$u.cpp"
done
echo "host: ${#units[@]} units compile"
if [[ -n ${VITASDK:-} && -d $VITASDK/arm-vita-eabi/include/c++/15.2.0 ]]; then
  v=$VITASDK/arm-vita-eabi
  vita=(--target=arm-vita-eabi --sysroot="$v" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fno-short-enums -femulated-tls -fno-pic
        -munaligned-access -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -nostdlibinc -nostdinc++ -isystem "$v/include/c++/15.2.0"
        -isystem "$v/include/c++/15.2.0/arm-vita-eabi" -isystem "$v/include/c++/15.2.0/backward" -isystem "$v/include" -Ivita/include)
  for u in "${units[@]}"; do
    nice /usr/bin/clang++ -std=gnu++20 "${vita[@]}" "${inc[@]}" "${common[@]}" -include vita/include/vita_source_prelude.h \
      -include platform/include/port/compat.h -Wno-builtin-macro-redefined -fsyntax-only "decomp/src/Game/$u.cpp"
  done
  echo "vita: ${#units[@]} units compile"
fi

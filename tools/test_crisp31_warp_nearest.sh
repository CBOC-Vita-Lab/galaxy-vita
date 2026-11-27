#!/usr/bin/env bash
# crisp31: GXV_HRW_WARP_NEAREST (the 60 Assist warp with point reads), the production warp program on Mesa (tests/crisp31_warp_nearest.cpp).
# Part 4: GXV_HRW_WARP_SNAP (the pairs the rule snaps are the first picture exactly).
# Needs Mesa EGL / GLESv2 (headers: CPLUS_INCLUDE_PATH=/work/khr/include where they are not installed). Exit 0 when every check passes.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The real kWarpBox, kVsWarp and WarpFragment, not a copy of their GLSL.
awk '
/^constexpr int kWarpBox =/ { print; n++ }
/^const char\* const kVsWarp =$/ { region=1; n++ }
/^std::string WarpFragment\(/ { region=2; n++ }
region { print }
region==1 && /";[[:space:]]*$/ { region=0 }
region==2 && /^}/ { region=0 }
END { if (n!=3 || region) exit 1 }
' platform/vgx/src/vgx_backend_gl.cpp > "$tmp/warp_sources.inc"
sed -i '1i static int WarpSharpEnv() { return 0; }  // (the back end reads GXV_HRW_WARP_SHARP, default 0 = off)' "$tmp/warp_sources.inc"
# finish44, GXV_HRW_WARP_SNAP: the test's part 4 uses the rule the back end uses; the test is void if DrawWarp stops using it this way.
grep -qF 'const bool snapPair = !WarpNearestEnv() && WarpSnapEnv() > 0 && port::hrw41::SnapPair(W.maxMovePx, WarpSnapEnv() * 0.1);' platform/vgx/src/vgx_backend_gl.cpp \
  && grep -qF 'const GLuint nearSrc = (WarpNearestEnv() || snapPair) ? (W.sceneOk ? W.texScene : W.tex) : 0u;' platform/vgx/src/vgx_backend_gl.cpp \
  || { echo "FAILED: DrawWarp's GXV_HRW_WARP_SNAP rule changed: update tests/crisp31_warp_nearest.cpp part 4"; exit 1; }
"${CXX:-c++}" -std=c++17 -O1 -I"$tmp" -Iplatform/include tests/crisp31_warp_nearest.cpp -lEGL -lGLESv2 -o "$tmp/check"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$tmp/check" "$@"

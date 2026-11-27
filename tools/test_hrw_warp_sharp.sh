#!/usr/bin/env bash
# finish44: GXV_HRW_WARP_SHARP (hrwfeel36, HRW-DESIGN S3a): the production depth-warp program with the sharp read, on Mesa, against port::hrw41::SharpCoord
# (tests/hrw_warp_sharp_gl.cpp). Needs Mesa EGL / GLESv2 (libegl-dev, libgles-dev). Exit 0 when every check passes.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The real kVsWarp and D41Fragment, not a copy of their GLSL.
awk '
/^const char\* const kVsWarp =$/ { region=1; n++ }
/^std::string D41Fragment\(/ { region=2; n++ }
region { print }
region==1 && /";[[:space:]]*$/ { region=0 }
region==2 && /^}/ { region=0 }
END { if (n!=2 || region) exit 1 }
' platform/vgx/src/vgx_backend_gl.cpp > "$tmp/sharp_sources.inc"
# The uniforms as DrawWarp sets them: the test's Warp() mirrors these two lines; it is void if they change.
grep -qF 'G(glUniform4f(P.sh, static_cast<float>(W.w), static_cast<float>(W.h), WarpSharpEnv() * 0.1f, 0.0f));' platform/vgx/src/vgx_backend_gl.cpp \
  && grep -qF 'G(glUniform4f(P.shT, 1.0f / static_cast<float>(W.w), 1.0f / static_cast<float>(W.h), 0.0f, 0.0f));' platform/vgx/src/vgx_backend_gl.cpp \
  || { echo "FAILED: DrawWarp's GXV_HRW_WARP_SHARP uniforms changed: update tests/hrw_warp_sharp_gl.cpp"; exit 1; }
"${CXX:-c++}" -std=c++17 -O1 -I"$tmp" -Iplatform/include tests/hrw_warp_sharp_gl.cpp -lEGL -lGLESv2 -o "$tmp/check"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$tmp/check" "$@"

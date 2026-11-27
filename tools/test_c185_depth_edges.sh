#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '
/^constexpr int kWarpBox =/ { print; n++ }
/^const char\* const (kVs|kVsWarp|kVsBoxKey|kFsBoxKey|kFsBoxDepth|kFsMask) =$/ { region=1; n++ }
/^std::string (WarpFragment|D41Fragment|MaskFragment)\(/ { region=2; n++ }
region { print }
region==1 && /";[[:space:]]*$/ { region=0 }
region==2 && /^}/ { region=0 }
END { if (n!=10 || region) exit 1 }
' platform/vgx/src/vgx_backend_gl.cpp > "$tmp/warp_sources.inc"
sed -i '1i static int WarpSharpEnv() { return 0; }  // (the back end reads GXV_HRW_WARP_SHARP, default 0 = off)' "$tmp/warp_sources.inc"
"${CXX:-c++}" -std=c++17 -O2 -DC185_DEPTH_EDGES -I"$tmp" -Iplatform/include tests/c182_warp_pixels.cpp -lEGL -lGLESv2 -o "$tmp/check"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$tmp/check" "$@"

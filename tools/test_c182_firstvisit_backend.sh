#!/usr/bin/env bash
set -euo pipefail
# Needs the Khronos GLES2 headers and libGLESv2 (no GL context is made). On the cloud host: CPATH=/work/khr/include CXX=clang++-18 (g++ stops at a duplicate extern "C" declaration).
#   bash tools/test_c182_firstvisit_backend.sh [VPK | vgx_stagewarm.bin]   (seedrun31b: with a packaged list, it is tagged by tools/seedrun/stagetags.py and loaded
#   through the production StageLoadPackaged / StageBuild: the historical records must be the head of AstroGalaxy's and HoneyBeeKingdomGalaxy's queues)
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [[ $# == 1 ]]; then
    case "$1" in
        *.vpk) unzip -p "$1" 'shadercache/vgx_stagewarm.bin' > "$tmp/stage.bin" ;;
        *) cp "$1" "$tmp/stage.bin" ;;
    esac
    python3 -I tools/seedrun/stagetags.py legacy "$tmp/stage.bin" "$tmp/stage-tagged.bin"
    export FV_REAL_TAGGED="$tmp/stage-tagged.bin"
fi
"${CXX:-c++}" -std=c++20 -O1 -DVGX_NO_NEON -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party tests/c182_firstvisit_backend.cpp platform/vgx/src/vgx_autoq.cpp platform/vgx/src/vgx_shadergen.cpp \
    -lGLESv2 -pthread -o "$tmp/check"
GXV_STAGEQ_SCENARIO=1 "$tmp/check"   # (the hints on: seedrun31's cases)
"$tmp/check"                         # (fix31: the default, GXV_STAGEQ_SCENARIO=0: the order before seedrun31)

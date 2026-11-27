#!/usr/bin/env bash
# tests/vgx_stage_warm.cpp (GXV_STAGEWARM: records, the stage file, the package list, the program table reserve, the evictions, the player file) on the
# Vita GL stub (armhf/glstub + tests/asynclink_glshim.h, as tools/test_c185_astro_mask.sh builds the backend).
#   bash tools/test_vgx_stage_warm.sh          build it (the gate: the test must keep compiling against the backend)
#   bash tools/test_vgx_stage_warm.sh --run    build and run it
# fix31: it did not compile since C182 (the GL stub lacked names; smooth23's rebuild counters and the built-in player seed are gone) and no script built it.
# It compiles again. harvprep31: its play model knows the C182/C183 load (a stage announced in play waits for a frame that is not live, GXV_STAGEBUILD_DEFER;
# the load's black frame, GXV_LOAD_BLACK, builds it; port_scene_loading_active is "a load runs", not "a stage is named") and the whole run passes; `stage-warm
# seedwalk` (run too) checks the harvest's per-stage cap and draw evidence (GXV_SEEDWALK=1).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/stage-warm.XXXXXX"); trap 'rm -rf "$out"' EXIT
cxx=${CXX:-}
if [[ -z $cxx ]]; then if command -v clang++ >/dev/null; then cxx=clang++; elif command -v clang++-18 >/dev/null; then cxx=clang++-18; else cxx=c++; fi; fi
src=$root/platform/vgx/src
"$cxx" -std=c++17 -O1 -DNDEBUG -fpermissive -w -D__VGX_VITA__=1 -I"$root/armhf/glstub" -I"$root/tests" -I"$root/tests/autoq_clock_stubs" -I"$root/platform/include" \
    -I"$src" -I"$root/platform/vgx/third_party" "$root/tests/vgx_stage_warm.cpp" "$src/vgx_shadergen.cpp" "$src/vgx_autoq.cpp" -lpthread -o "$out/stage-warm"
echo "vgx_stage_warm: builds"
if [[ ${1:-} == --run ]]; then (cd "$out" && ./stage-warm && ./stage-warm seedwalk); fi

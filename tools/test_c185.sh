#!/usr/bin/env bash
# macOS runs the shared host suite; C185_DEPTH_EGL=1 also checks real GLSL on Linux/Mesa.
set -euo pipefail
cd "$(dirname "$0")/.."
if (( $# > 1 )); then printf 'usage: bash %s [game-dir]\n' "$0" >&2; exit 2; fi
plant_args=()
if (( $# >= 1 )); then plant_args+=("$1/files/ObjectData"); fi
bash tools/test_c185_plant.sh "${plant_args[@]}"
if [[ $(uname -s) == Linux ]]; then
    bash tools/test_c185_astro_mask.sh
else
    printf 'Astro exact-mask backend: separate Linux GL-stub gate (tools/test_c185_astro_mask.sh)\n'
fi
if [[ ${C185_DEPTH_EGL:-0} == 1 ]]; then
    bash tools/test_c185_depth_edges.sh
else
    printf 'Depth-edge GLSL pixels: separate Mesa gate (tools/test_c185_depth_edges.sh)\n'
fi
bash tools/test_c184.sh "$@" # (c185merge: the Honeyhive GXV_VSOUT=0 recipe supplement and its tests are left out)

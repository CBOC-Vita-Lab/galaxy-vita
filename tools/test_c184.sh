#!/usr/bin/env bash
# Supply the private cooked game/ directory for the full data-dependent gates.
# No game data enter the source.
set -euo pipefail
cd "$(dirname "$0")/.."
if (( $# > 1 )); then printf 'usage: bash %s [game-dir]\n' "$0" >&2; exit 2; fi
bash tools/test_c184_archive.sh
node tools/test_c184_depth_plan.mjs
if (( $# >= 1 )); then
    bash tools/test_j3d_envelope_neon.sh "$1/files/ObjectData" --all
else
    bash tools/test_j3d_envelope_neon.sh
fi
bash tools/test_c183.sh "${@:1:1}"

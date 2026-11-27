#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=decomp/src/Game/MapObj/PlantGroup.cpp
holder=decomp/src/Game/LiveActor/ClippingActorHolder.cpp
sed -n '/^void PlantGroup::portSplitDecorReveal(/,/^}/p' "$src" > "$work/plant.inc"
test -s "$work/plant.inc"
c++ -std=c++17 -Wall -Wextra -Werror -I"$work" tests/c185_plant.cpp -o "$work/check"
"$work/check" | tee "$work/behaviour.txt"
python3 - "$src" "$holder" <<'PY'
from pathlib import Path
import re, sys
plant, holder = (Path(p).read_text() for p in sys.argv[1:])
assert re.search(r'pActor->endClipped\(\);\s*if \(wasPbFar && !pbFar && pbDist > 0.0f && pInfo->mPbDecor\)\s*\{\s*static_cast< PlantGroup\* >\(pActor\)->portSplitDecorReveal\(pbCam, tinyNext\);', holder)
assert plant.count('->mPbDecor = true;') == 1
assert 'itemCount == 0' in plant and 'MR::isEqualStageName("HoneyBeeKingdomGalaxy")' in plant
assert 'MR::isEqualString(name, "CutBushGroup")' in plant and 'MR::isEqualString(name, "FlowerBlueGroup")' in plant
assert 'getenv("GXV_HONEY_PLANT_REVEAL")' in plant
draw = Path('decomp/src/Game/System/DrawBuffer.cpp').read_text()
assert 'getModel()->mPortCullPicture == cullPicture' in draw
assert 'kFrozenCulled) != 0' in draw
print("Real PB release, authored category/stage/rollback, live and frozen packet contracts: PASS")
PY
if (( $# )); then
    "${PYTHON:-python3}" tools/c185_plant_pixels.py "$1" "$work/behaviour.txt" "${@:2}"
fi

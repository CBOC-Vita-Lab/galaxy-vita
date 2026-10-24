#!/bin/bash
# Boots into each listed galaxy (PETARI_STAGE) on the headset, headless, and
# collects two captures plus the exit status per galaxy under out/tour/.
#   tools/galaxy_tour.sh [Galaxy[:scenario] ...]
# Needs an existing save file on slot 1 (the menu script selects it).
# HEADLESS_DATA and HEADLESS_NAND as for run_headless.sh.
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
source tools/env.sh
ADB=$(find_adb) || { echo "adb not found" >&2; exit 1; }
SHOTS=$(dirname ${HEADLESS_NAND:-/data/local/tmp/petari/nand})/shots
GALAXIES=("$@")
if [ ${#GALAXIES[@]} -eq 0 ]; then
  GALAXIES=(AstroGalaxy HeavensDoorGalaxy EggStarGalaxy HoneyBeeKingdomGalaxy CosmosGardenGalaxy BattleShipGalaxy
            TriLegLv1Galaxy HeavenlyBeachGalaxy PhantomGalaxy IceVolcanoGalaxy SandClockGalaxy ReverseKingdomGalaxy
            OceanRingGalaxy FactoryGalaxy HellProminenceGalaxy KoopaBattleVs1Galaxy DarkRoomGalaxy OceanFloaterLandGalaxy
            SurfingLv1Galaxy CannonFleetGalaxy)
fi
mkdir -p out/tour
PRESSES=$(for t in $(seq 44 3 77); do printf ",%s-%s.2:A" $t $t; done)
LATE=$(for t in $(seq 95 4 125); do printf ",%s-%s.2:A" $t $t; done)
MENU="28-28.4:A|B,30-36:PX=-0.25|PY=0.27,33-33.2:A,36-42:PX=0.52|PY=0.78,38-38.2:A"
: > out/tour/summary.txt
first=1
for g in "${GALAXIES[@]}"; do
  name=${g%%:*}
  push=""
  if [ $first -eq 0 ]; then push="--no-push"; fi
  first=0
  PETARI_STAGE="$g" PETARI_INPUT="$MENU$PRESSES$LATE" PETARI_SHOT_MS=15000 tools/run_headless.sh 130 $push > out/tour/$name.log 2>&1
  status=$(grep -o 'exit status [0-9]*' out/tour/$name.log | tail -1)
  draws=$(grep 'headless: frame' out/tour/$name.log | tail -1 | sed -E 's/.*\(([0-9]+) draws.*/\1/')
  crash=$(sed -n '/=== crash ===/,$p' out/tour/$name.log | grep -m1 -E '^[A-Za-z_:~<>]+.*\(' | cut -c1-120)
  echo "$name: $status, last frame $draws draws ${crash:+CRASH: $crash}" | tee -a out/tour/summary.txt
  rm -f out/tour/${name}_*.png
  for f in 006 007; do
    $ADB pull $SHOTS/frame_$f.png out/tour/${name}_$f.png > /dev/null 2>&1
  done
done

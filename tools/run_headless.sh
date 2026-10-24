#!/bin/bash
# Pushes the current build to the headset and boots it headless.
#   tools/run_headless.sh [seconds] [--no-push]
# On a crash, prints the symbolized backtrace from the crash log buffer.
# HEADLESS_DATA: game files on the headset other than the app's own (another
# region's disc).  HEADLESS_NAND: the save folder; the snapshots go to shots/
# next to it.
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
source tools/env.sh
ADB=$(find_adb) || { echo "adb not found" >&2; exit 1; }
NDKBIN=$(ndk_llvm_bin)
SECS=${1:-20}
DEV=/data/local/tmp/petari
DATA=${HEADLESS_DATA:-/sdcard/Android/data/com.galaxy.quest/files/game}
NAND=${HEADLESS_NAND:-$DEV/nand}
SHOTS=$(dirname $NAND)/shots

if [ "$2" != "--no-push" ]; then
  mkdir -p out/headless
  $NDKBIN/llvm-strip$EXE --strip-unneeded -o out/headless/libgame.so build-android/libgame.so
  cp build-android/petari_headless out/headless/
  $ADB shell mkdir -p $DEV/nand
  $ADB push out/headless/libgame.so out/headless/petari_headless $DEV/ > /dev/null || exit 1
  $ADB shell chmod 755 $DEV/petari_headless
fi

ENVS=""
for v in PETARI_XRSIM_MV PETARI_PERFLOG PETARI_XRSIM_FPS PETARI_AUDIOLOG PETARI_COPYDUMP PETARI_STAGE PETARI_MOVIE PETARI_CUTTEST PETARI_WANDER PETARI_XRSIM PETARI_XRHEAD PETARI_FOVEATE PETARI_NOCOPY PETARI_VRSIZE PETARI_WAV PETARI_ASYNC PETARI_VRTEST PETARI_VRHEAD PETARI_GLITEMS PETARI_GLSTEPS PETARI_INPUT PETARI_DUMP PETARI_THREADS PETARI_NERVES PETARI_GLDBG PETARI_GLSRC PETARI_GLVIS PETARI_GLEXEC PETARI_GLSTOP PETARI_SHOT_MS PETARI_SHOT_FROM PETARI_WARP PETARI_SELOG PETARI_DSPLOG PETARI_SETEST PETARI_SWITCH PETARI_STARBITS PETARI_XRSIM_STEPS PETARI_XRSIM_SMALL PETARI_XRSIM_BUDGET PETARI_RIGLOG PETARI_CAMLOG PETARI_POSLOG PETARI_VRINI PETARI_XRSIM_AIM PETARI_VRLIGHT_OFF PETARI_XRSIM_SWAPSCALE PETARI_FIXED_SCALE PETARI_XRSIM_LAYERS PETARI_XRSIM_TURN PETARI_DLCHECK PETARI_NODLCACHE PETARI_LANGUAGE PETARI_PANELLOG; do
  if [ -n "${!v}" ]; then ENVS="$ENVS $v='${!v}'"; fi
done

$ADB shell "mkdir -p $NAND $SHOTS && rm -f $SHOTS/*.png"
$ADB logcat -b crash -c
$ADB shell "cd $DEV &&$ENVS LD_LIBRARY_PATH=$DEV timeout $((SECS + 10)) ./petari_headless $DATA $NAND $SECS; echo \"[exit status \$?]\"" 2>&1 | tee out/headless/last_run.txt

# Symbolize thread dumps (" g:<offset>" entries are libgame.so offsets).
if grep -q "  bt:" out/headless/last_run.txt; then
  echo "=== thread backtraces ==="
  grep -E "^thread |^threads:|  bt:" out/headless/last_run.txt | while IFS= read -r line; do
    case "$line" in
      *"bt:"*)
        offs=$(echo "$line" | grep -oE "g:[0-9a-f]+" | sed 's/g:/0x/' | head -14)
        if [ -n "$offs" ]; then
          $NDKBIN/llvm-addr2line$EXE -C -f -e build-android/libgame.so $offs | sed -E "s|$(native_path "$REPO")/||" | paste - - | sed -E 's/^/    /'
        fi
        ;;
      *) echo "$line" ;;
    esac
  done
fi

CRASH=$($ADB logcat -b crash -d)
if echo "$CRASH" | grep -q "Fatal signal"; then
  echo "=== crash ==="
  echo "$CRASH" | grep -E "Fatal signal|signal [0-9]+|Abort message|x0 |x4 |x8 |x12 |x16 |x20 |x24 |x28 |lr " | head -12
  PCS=$(echo "$CRASH" | grep -E "#[0-9]+ pc [0-9a-f]+ +/data/local/tmp/petari/libgame.so" | sed -E 's/.*pc ([0-9a-f]+).*/0x\1/' | head -24)
  if [ -n "$PCS" ]; then
    $NDKBIN/llvm-addr2line$EXE -C -f -i -e build-android/libgame.so $PCS | sed -E "s|$(native_path "$REPO")/||" | paste - - | sed -E "s/	/   /"
  fi
fi

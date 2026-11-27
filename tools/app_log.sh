#!/bin/bash
# Collects the VR app's log after a session in the headset:
#   tools/app_log.sh            the app's log (tag PetariVR) and, if it
#                               crashed, the symbolized backtrace
#   tools/app_log.sh --clear    empty the logs before a new session
# The output is also saved to out/app_log.txt.  The app's own log file
# (petari_log.txt next to the game data, the session before as .prev) is
# copied to out/ too: the system log only keeps a few minutes of a session.
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
source tools/env.sh
ADB=$(find_adb) || { echo "adb not found" >&2; exit 1; }
NDKBIN=$(ndk_llvm_bin)

if [ "$1" = "--clear" ]; then
  $ADB logcat -c
  $ADB logcat -b crash -c
  echo "logs cleared"
  exit 0
fi

mkdir -p out
FILES=/sdcard/Android/data/com.galaxy.quest/files
$ADB pull $FILES/petari_log.txt out/petari_log.txt > /dev/null 2>&1
$ADB pull $FILES/petari_log.txt.prev out/petari_log_prev.txt > /dev/null 2>&1
{
  echo "=== PetariVR log ==="
  LOG=$($ADB logcat -d -s PetariVR:V OpenXR:V)
  if ! echo "$LOG" | grep -q PetariVR && [ -f out/petari_log.txt ]; then
    echo "(the system log no longer has the session: the app's log file, out/petari_log.txt)"
    LOG=$(cat out/petari_log.txt)
  fi
  echo "$LOG" | tail -400
  CRASH=$($ADB logcat -b crash -d)
  if echo "$CRASH" | grep -q "Fatal signal"; then
    echo "=== crash ==="
    echo "$CRASH" | grep -E "Fatal signal|Abort message|x0 |x4 |x8 |lr " | head -8
    # Frames in libgame.so (installed as part of the APK).
    PCS=$(echo "$CRASH" | grep -E "#[0-9]+ pc [0-9a-f]+ +.*libgame.so" | sed -E 's/.*pc ([0-9a-f]+).*/0x\1/' | head -24)
    if [ -n "$PCS" ]; then
      $NDKBIN/llvm-addr2line$EXE -C -f -i -e build-android/libgame.so $PCS | sed -E "s|$(native_path "$REPO")/||" | paste - - | sed -E "s/	/   /"
    fi
  fi
} | tee out/app_log.txt

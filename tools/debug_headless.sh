#!/bin/bash
# Runs the headless boot under lldb (remote, via lldb-server on the headset).
#   tools/debug_headless.sh <lldb command file> [seconds]
# The unstripped libgame.so is pushed so lldb has full symbols.  The command
# file runs after the process is launched and stopped at entry; it usually
# sets breakpoints, `continue`s and inspects state.
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
source tools/env.sh
ADB=$(find_adb) || { echo "adb not found" >&2; exit 1; }
NDK=$(find_ndk)
LLDB=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/lldb.cmd
DEV=/data/local/tmp/petari
DATA=/sdcard/Android/data/com.galaxy.quest/files/game
CMDS=$1
SECS=${2:-30}

$ADB push build-android/libgame.so build-android/petari_headless $DEV/ > /dev/null || exit 1
$ADB push $NDK/toolchains/llvm/prebuilt/windows-x86_64/lib/clang/21/lib/linux/aarch64/lldb-server $DEV/ > /dev/null
$ADB shell chmod 755 $DEV/lldb-server $DEV/petari_headless
$ADB shell "pkill -f lldb-server; pkill -f petari_headless" 2>/dev/null
$ADB forward tcp:5039 tcp:5039 > /dev/null
$ADB shell "cd $DEV && LD_LIBRARY_PATH=$DEV ./lldb-server gdbserver :5039 -- ./petari_headless $DATA $DEV/nand $SECS > $DEV/lldb-server.log 2>&1 &" &
sleep 2

cat > out/lldb_init.txt <<EOF
settings set target.max-string-summary-length 200
gdb-remote localhost:5039
process handle SIGSEGV -s true -p false -n true
command source $(cygpath -w "$CMDS" | sed 's/\\/\\\\/g')
EOF
cmd //c "$(cygpath -w $LLDB) -b -s $(cygpath -w out/lldb_init.txt)" 2>&1 | grep -v "^(lldb) *$"
$ADB shell "pkill -f lldb-server; pkill -f petari_headless" 2>/dev/null

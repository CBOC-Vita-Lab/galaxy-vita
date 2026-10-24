#!/system/bin/sh
# Android starts a debuggable app through lib/<abi>/wrap.sh when the APK has
# one: the app process is exec'd ("$@" is app_process64 and its arguments)
# instead of forked from the zygote.  libreserve.so reserves the game's 32-bit
# address window before ART lays out its heap (see reserve.c).
#
# Options right after app_process64 go to ART.  With the window taken, the low
# 4 GB has no 512 MB gap left for ART's default large object space (it wants
# one the size of dalvik.vm.heapsize), so large Java objects go to the main
# heap instead; this app hardly allocates any.
#
# Packaged with LF line endings by tools/package_apk.sh.
export LD_PRELOAD="${0%/*}/libreserve.so${LD_PRELOAD:+:$LD_PRELOAD}"
app_process="$1"
shift
exec "$app_process" -XX:LargeObjectSpace=disabled "$@"

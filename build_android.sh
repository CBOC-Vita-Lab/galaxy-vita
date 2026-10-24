#!/bin/bash
# Configures and builds the Quest (Android arm64) target: build-android/
# libgame.so (the game), libmain.so (the launcher), libreserve.so and the
# petari_headless test runner.  tools/package_apk.sh then makes the APK.
#   ./build_android.sh [ninja targets...]
# Needs the Android NDK r29, CMake 3.24+ and Ninja (see tools/env.sh for how
# they are found).  The OpenXR loader is downloaded on the first build
# (tools/fetch_openxr.sh).
set -e
cd "$(dirname "$0")"
source tools/env.sh

NDK=$(find_ndk) || { echo "Android NDK not found: set ANDROID_NDK_HOME (NDK r29)" >&2; exit 1; }
CMAKE_BIN=$(find_cmake) || { echo "cmake not found: put it on PATH or set CMAKE" >&2; exit 1; }
NINJA_BIN=$(find_ninja) || { echo "ninja not found: put it on PATH or set NINJA" >&2; exit 1; }
[ -f third_party/openxr/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so ] || tools/fetch_openxr.sh

BUILD=${BUILD:-build-android}
if [ ! -f $BUILD/build.ninja ]; then
  "$CMAKE_BIN" -S . -B $BUILD -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$(native_path "$NINJA_BIN")" \
    -DCMAKE_TOOLCHAIN_FILE="$(native_path "$NDK")/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 \
    -DCMAKE_BUILD_TYPE=${CONFIG:-RelWithDebInfo}
fi
"$NINJA_BIN" -C $BUILD "$@"

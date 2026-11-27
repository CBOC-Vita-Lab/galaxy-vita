#!/bin/bash
# Downloads the Khronos OpenXR loader for Android (Apache License 2.0) from
# Maven Central into third_party/openxr: its headers, which the game is
# compiled against, and libopenxr_loader.so, which the APK ships.
set -e
cd "$(dirname "$0")/.."
source tools/env.sh

VERSION=1.1.63
URL=https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$VERSION/openxr_loader_for_android-$VERSION.aar
DEST=third_party/openxr

mkdir -p $DEST
echo "fetching the OpenXR loader $VERSION"
curl -fsSL --retry 3 -o $DEST/openxr_loader.aar "$URL"
# An .aar is a zip file.
py3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$(native_path $DEST/openxr_loader.aar)" "$(native_path $DEST)"
rm $DEST/openxr_loader.aar
test -f $DEST/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so
echo "OpenXR loader in $DEST"

#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
platform=${GALAXY_NATIVE_PLATFORM:-linux/arm64}
image=${GALAXY_NATIVE_IMAGE:-codex-galaxy-native:20261007-arm64}
build=${GALAXY_NATIVE_BUILD:-build-native-arm64}
jobs=${GALAXY_NATIVE_JOBS:-4}
case "$build" in build-*) ;; *) echo 'Build directory must start with build-' >&2; exit 2 ;; esac
[[ "$build" != */* && "$jobs" =~ ^[1-9][0-9]*$ ]] || exit 2

docker build --platform "$platform" -t "$image" -f tools/Dockerfile.native .
docker run --rm --platform "$platform" --network none \
  -v "$root:/work" -w /work "$image" cmake -S . -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
docker run --rm --platform "$platform" --network none \
  -v "$root:/work" -w /work "$image" nice -n 10 cmake --build "$build" -j "$jobs"

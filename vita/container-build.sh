#!/usr/bin/env bash
set -euo pipefail
jobs=${VITA_SOURCE_JOBS:-2}
[[ "$jobs" =~ ^[1-8]$ ]] || exit 2

# Installs affect this disposable container only, never the shared base image.
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
  apt-get update > /build/apt-update.log 2>&1
  apt-get install -y --no-install-recommends cmake ninja-build > /build/apt-install.log 2>&1
fi
{
  clang --version
  cmake --version
  ninja --version
  llvm-ar --version
} > /build/tool-versions.txt
printf '%s\n' 'cmake -U CMAKE_C_FLAGS -U CMAKE_CXX_FLAGS -S /source/vita -B /build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/source/vita/clang-vita-toolchain.cmake -DVITASDK=/opt/vitasdk -DCMAKE_BUILD_TYPE=Release' "nice -n 10 ninja -C /build -j$jobs -k0 galaxy_vita_sources" > /build/container-commands.txt
# Reset initialized toolchain flags, not CMakeFiles (which contains the objects).
# Ninja then rebuilds changed commands/dependencies while retaining valid work.
cmake -U CMAKE_C_FLAGS -U CMAKE_CXX_FLAGS -S /source/vita -B /build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/source/vita/clang-vita-toolchain.cmake \
  -DVITASDK=/opt/vitasdk -DCMAKE_BUILD_TYPE=Release 2>&1 | tee /build/configure.log
python3 -m unittest discover -s /source/vita -p 'test_*.py'
python3 /source/vita/standalone_compile.py abi > /build/abi-check.log 2>&1
if [ "${1:-full}" = configure ]; then
  exit 0
fi
python3 /source/vita/standalone_compile.py boundary > /build/boundary-check.log 2>&1
python3 /source/vita/build_report.py snapshot /source /build/source-state-before.json
set +e
nice -n 10 ninja -C /build -j"$jobs" -k0 galaxy_vita_sources 2>&1 | tee /build/build.log
status=${PIPESTATUS[0]}
set -e
python3 /source/vita/build_report.py /source /build "$status"
exit "$status"

#!/usr/bin/env bash
# Build clang's profile runtime (libclang_rt.profile) for armhf Linux from the LLVM 18.1.3 compiler-rt sources.
# The Ubuntu clang package only ships the x86 runtimes.  Needs clang 18, llvm-ar, curl, xz and network access
# (or SRC_TARBALL=<compiler-rt-18.1.3.src.tar.xz>).
#
#   tools/pgo/build_profile_rt.sh <out-dir>     ->  <out-dir>/libclang_rt.profile-armhf.a
set -euo pipefail
out=${1:?usage: build_profile_rt.sh <out-dir>}
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.3/compiler-rt-18.1.3.src.tar.xz
sha=9a7df9300413696b0c4f7ff1e2729cb82aca375f35c05d698c44f26a4edf1c27
mkdir -p "$out/src"
tar_file=${SRC_TARBALL:-$out/compiler-rt.tar.xz}
[ -f "$tar_file" ] || curl -sSL -o "$tar_file" "$url"
echo "$sha  $tar_file" | sha256sum -c -
tar -xJf "$tar_file" -C "$out/src" --strip-components=1 compiler-rt-18.1.3.src/lib/profile compiler-rt-18.1.3.src/include/profile
P=$out/src/lib/profile
CF=(--target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fPIC -fno-exceptions
    -DCOMPILER_RT_HAS_UNAME=1 -I"$out/src/include" -I"$P" -fvisibility=hidden -Wno-unused-function)
objs=()
for f in InstrProfiling InstrProfilingBuffer InstrProfilingFile InstrProfilingInternal InstrProfilingMerge \
         InstrProfilingMergeFile InstrProfilingNameVar InstrProfilingPlatformLinux InstrProfilingPlatformOther \
         InstrProfilingUtil InstrProfilingValue InstrProfilingWriter InstrProfilingVersionVar; do
  clang "${CF[@]}" -c "$P/$f.c" -o "$out/$f.o"; objs+=("$out/$f.o")
done
clang++ "${CF[@]}" -std=c++17 -fno-rtti -c "$P/InstrProfilingRuntime.cpp" -o "$out/InstrProfilingRuntime.o"; objs+=("$out/InstrProfilingRuntime.o")
rm -f "$out/libclang_rt.profile-armhf.a"
llvm-ar rcs "$out/libclang_rt.profile-armhf.a" "${objs[@]}"
echo "$out/libclang_rt.profile-armhf.a"

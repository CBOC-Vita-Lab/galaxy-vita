#!/usr/bin/env bash
# hotprof25 round 2 (11 Nov): GXV_HOTPROF_TEVCOL and GXV_HOTPROF_REINITGX on the host: the production GX units, the capture layer, a FIFO that
# stands for the stream and a model of the front end's registers (tests/hotprof_gx.cpp says what is compared). Direct, then with the sequence
# cut into GX captures (GXV_GXCAPTURE=1, and =2: the layer's own verify must report nothing), then an ASan + UBSan build.
#   tools/test_hotprof_gx.sh [seeds]        (build in $OUT, default a temporary directory, removed at the end)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
seeds=${1:-300}
out=${OUT:-$(mktemp -d)}
[[ -n "${OUT:-}" ]] || trap 'rm -rf "$out"' EXIT
mkdir -p "$out"
CC=${CC:-clang}; CXX=${CXX:-clang++}
inc=(-I"$root/platform/include" -I"$root/decomp/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/JSystem/include")
cflags=(-std=gnu11 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include "$root/platform/include/port/compat.h" -w
        -Wno-implicit-function-declaration)
cxxflags=(-std=c++17 -fno-strict-aliasing -fms-extensions -DTARGET_PC=1 "${inc[@]}" -include string -include vector
          -include "$root/platform/include/port/compat.h" -w -pthread)
build() { # build <name> <extra flags...>
  local name=$1; shift
  local o="$out/$name"; mkdir -p "$o"
  local objs=()
  for u in GXAttr GXBump GXDisplayList GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPixel GXTev GXTexture GXTransform; do
    "$CC" "${cflags[@]}" "$@" -c "$root/decomp/src/RVL_SDK/gx/$u.c" -o "$o/$u.o"; objs+=("$o/$u.o")
  done
  "$CC" "${cflags[@]}" "$@" -c "$root/platform/src/sdk/mtx.c" -o "$o/mtx.o"
  "$CC" "${cflags[@]}" "$@" -c "$root/tests/gx_capture_stubs.c" -o "$o/stubs.o"
  "$CXX" "${cxxflags[@]}" "$@" -c "$root/platform/src/gx/gx_capture.cpp" -o "$o/gx_capture.o"
  "$CXX" "${cxxflags[@]}" "$@" -c "$root/tests/hotprof_gx.cpp" -o "$o/test.o"
  "${LINK:-$CXX}" "$@" -pthread "$o/test.o" "$o/gx_capture.o" "$o/stubs.o" "$o/mtx.o" "${objs[@]}" -Wl,--allow-multiple-definition -o "$out/$name.bin"
}
fail=0
run() { # run <label> <binary> [env...]
  local label=$1 bin=$2; shift 2
  if env "$@" "$bin" "$seeds" > "$out/$label.log" 2>&1; then echo "PASS $label"; grep -v '^hotprof_gx: PASS' "$out/$label.log" | sed 's/^/     /'
  else echo "FAIL $label"; tail -20 "$out/$label.log"; fail=1; fi
}
build plain -O2 -g
run direct "$out/plain.bin" GXV_GXCAPTURE=0
run captures "$out/plain.bin" GXV_GXCAPTURE=1
run captures-verify "$out/plain.bin" GXV_GXCAPTURE=2
# (this host's clang has no sanitizer runtimes: compiled with clang, linked with g++, whose libasan / libubsan take clang's instrumentation: as tools/test_gx_capture.sh)
LINK=${LINK_CXX:-g++}
build asan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
run asan-direct "$out/asan.bin" GXV_GXCAPTURE=0 ASAN_OPTIONS=detect_leaks=0
run asan-captures "$out/asan.bin" GXV_GXCAPTURE=1 ASAN_OPTIONS=detect_leaks=0
[[ $fail == 0 ]] && echo "test_hotprof_gx: PASS" || { echo "test_hotprof_gx: FAIL"; exit 1; }

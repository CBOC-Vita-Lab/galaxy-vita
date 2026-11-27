#!/usr/bin/env bash
# asyncsh31: the shader worker allocates from the C heap only (platform/vgx/src/vgx_worker_alloc.h), fake allocators, plain + ASan/UBSan + TSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
for flavour in plain asan tsan; do
  case $flavour in
    plain) flags="-O2";;
    asan) flags="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer";;
    tsan) flags="-O1 -g -fsanitize=thread";;
  esac
  if ! "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror $flags -I"$root/platform/vgx/src" "$root/tests/vgx_worker_alloc.cpp" -o "$out/vgx_worker_alloc_$flavour" -lpthread 2>"$out/build_$flavour.txt"; then
    if [ "$flavour" != plain ] && grep -q "cannot find\|unrecognized\|not supported" "$out/build_$flavour.txt"; then echo "skip $flavour (no sanitizer runtime)"; continue; fi
    cat "$out/build_$flavour.txt"; exit 1
  fi
  echo "== $flavour"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out/vgx_worker_alloc_$flavour"  # (the fakes never free)
done
echo "ok: the shader worker allocates from the C heap only"

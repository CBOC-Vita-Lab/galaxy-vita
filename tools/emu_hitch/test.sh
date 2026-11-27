#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/emu-checkpoint.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
patch=$here/vita3k-92bda548.patch
header=vita3k/renderer/include/renderer/vulkan/checkpoint.h
# Compile the exact helper distributed inside the patch, not an approximation.
(cd "$tmp" && git apply --include="$header" "$patch")
cmp "$here/checkpoint.h" "$tmp/$header"
"${CXX:-c++}" -std=c++17 -O2 -pthread -Wall -Wextra -Werror -I"$tmp/$(dirname "$header")" \
    "$here/test.cpp" -o "$tmp/test"
"$tmp/test" "$tmp/cache"
if [ "$#" -gt 0 ]; then
    ref=92bda548f3aa0691102471a72725587e00676383
    test "$(git -C "$1" rev-parse HEAD)" = "$ref"
    # Use pristine tracked sources from the pin; do not mutate the supplied tree.
    mkdir "$tmp/upstream"
    git -C "$1" archive "$ref" vita3k/renderer | tar -xf - -C "$tmp/upstream"
    (cd "$tmp/upstream" && git apply --check "$patch" && git apply "$patch")
    cmp "$here/checkpoint.h" "$tmp/upstream/$header"
    printf '%s\n' "PASS full patch applies cleanly to Vita3K $ref."
fi

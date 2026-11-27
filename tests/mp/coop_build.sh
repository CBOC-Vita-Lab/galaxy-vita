#!/bin/sh
# Build and run the coop extras' host tests (greeting emote, pointer ping, super jump logic and codecs) with ASan + UBSan.
# Run from the repository root. clang++ compiles with -Werror as a compiler check; the sanitized binary is built with g++
# (this container's clang has no sanitizer runtime; CXX=clang++ to override).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/coop_test.cpp -o $MP_TMP/coop_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra \
  -I decomp/include -I platform/include tests/mp/coop_test.cpp -o $MP_TMP/coop_test
$MP_TMP/coop_test

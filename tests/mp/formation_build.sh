#!/bin/sh
# Build and run the puppet formation logic host tests (ASan + UBSan; g++ has the sanitizer runtime here, clang++ compiles the same
# file with -Werror as a second compiler check). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/formation_test.cpp -o $MP_TMP/formation_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra \
  -I decomp/include -I platform/include tests/mp/formation_test.cpp -o $MP_TMP/formation_test
$MP_TMP/formation_test

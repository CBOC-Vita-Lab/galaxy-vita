#!/bin/sh
# Build and run the lod2 host tests (decomp/include/Game/MP/MpLodLogic.hpp). ASan + UBSan with g++, clang++ -Werror as a second compiler.
# Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -c tests/mp/lod_test.cpp -o $MP_TMP/lod_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -I decomp/include tests/mp/lod_test.cpp -o $MP_TMP/lod_test
$MP_TMP/lod_test

#!/bin/sh
# Build and run the name tag layout host tests (ASan + UBSan with g++, clang++ -Werror as a second compiler). Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/tag_layout_test.cpp -o $MP_TMP/tag_layout_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -I decomp/include -I platform/include tests/mp/tag_layout_test.cpp -o $MP_TMP/tag_layout_test
$MP_TMP/tag_layout_test

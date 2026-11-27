#!/bin/sh
# Build and run the flow logic host tests with ASan + UBSan. Run from the repository root.
# clang++ is the preferred compiler; this container's clang has no sanitizer runtime, so the sanitized binary is built with g++
# (CXX=clang++ to override) and clang++ compiles the same file with -Werror as a second compiler check.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/flow_test.cpp -o $MP_TMP/flow_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra \
  -I decomp/include -I platform/include tests/mp/flow_test.cpp -o $MP_TMP/flow_test
$MP_TMP/flow_test

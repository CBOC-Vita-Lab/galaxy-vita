#!/bin/sh
# Build and run the costar module (team Star Bit count, Co-Star hold rules)
# Run from the repository root. clang++ compiles with -Werror as a compiler check; the sanitized binary is built with g++
# (this container's clang has no sanitizer runtime; CXX=clang++ to override).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/costar_test.cpp -o $MP_TMP/costar_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra \
  -I decomp/include -I platform/include tests/mp/costar_test.cpp -o $MP_TMP/costar_test
$MP_TMP/costar_test

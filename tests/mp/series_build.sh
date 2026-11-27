#!/bin/sh
# Build and run the race (series / catch-up / next step) host tests: decomp/include/Game/MP/MpSeriesLogic.hpp, MpFlowLogic.hpp, MpUi.hpp's RESULTS codec.
# ASan + UBSan with g++, clang++ -Werror as a second compiler. Run from the repo root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
CXX=${CXX:-g++}
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include -I platform/include -c tests/mp/series_test.cpp -o $MP_TMP/series_test_clang.o
$CXX -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -Wall -Wextra -I decomp/include -I platform/include tests/mp/series_test.cpp -o $MP_TMP/series_test
$MP_TMP/series_test

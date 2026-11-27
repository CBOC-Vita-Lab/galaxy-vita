#!/bin/sh
# Host test of the warm-up-hold / start-barrier decision (MpStartHold.hpp). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include tests/mp/starthold_test.cpp -o $MP_TMP/starthold_test
$MP_TMP/starthold_test

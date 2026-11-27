#!/bin/sh
# Host test of wait logic (uxwait). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include tests/mp/wait_test.cpp -o $MP_TMP/wait_test
$MP_TMP/wait_test

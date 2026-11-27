#!/bin/sh
# Host test of list_refresh logic (uxwait). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
clang++ -std=c++20 -Wall -Wextra -Werror -I decomp/include tests/mp/list_refresh_test.cpp -o $MP_TMP/list_refresh_test
$MP_TMP/list_refresh_test

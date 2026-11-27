#!/bin/bash
# Builds and runs the np_json.h limits + fuzz test under ASan/UBSan. Run through tools/buildrun.sh (it compiles).
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
g++ -std=c++17 -g -O1 -fsanitize=address,undefined -Inet/src -Inet/src/online tests/mp/json_limits_test.cpp -o $MP_TMP/json_limits_test
$MP_TMP/json_limits_test

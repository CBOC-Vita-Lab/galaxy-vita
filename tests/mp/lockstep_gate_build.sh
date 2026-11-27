#!/bin/bash
# Host test of the lockstep gate rules (platform/include/port/lockstep_gate.h). Run from the repository root.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
clang++ -std=c++20 -Wall -Wextra -Werror -Wno-sign-compare -I platform/include tests/mp/lockstep_gate_test.cpp -o $MP_TMP/lockstep_gate_test
$MP_TMP/lockstep_gate_test

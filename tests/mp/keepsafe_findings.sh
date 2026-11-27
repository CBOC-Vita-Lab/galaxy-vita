#!/bin/sh
# Runs the keepsafe audit's failing tests (findings for the owner of mp_config.cpp / mp_savefile.cpp). Exit 1 while any is open.
set -eo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
inc="-Iplatform/include -Iplatform/src/mp -Idecomp/include"
g++ -std=c++20 -g -O0 -Wno-unused-parameter $inc tests/mp/keepsafe_findings_test.cpp platform/src/mp/mp_config.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp platform/src/mp/mp_core_sys.cpp platform/src/mp/mp_core_vita.cpp -pthread -o $MP_TMP/keepsafe_findings 2>$MP_TMP/keepsafe_findings.err || { head -20 $MP_TMP/keepsafe_findings.err; exit 2; }
$MP_TMP/keepsafe_findings

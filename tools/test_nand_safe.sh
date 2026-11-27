#!/usr/bin/env bash
# (merge c99: nand.cpp also holds the multiplayer atomic writer, so mp_savefile.cpp / mp_savecheck.cpp are linked.)
# GXV_NAND_SAFE: the save's backup and put-back (platform/src/nand/nand.cpp), on the host (tests/nand_safe_test.cpp; forks a child that dies mid-write).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-/tmp/nand_safe}
mkdir -p "$out"
cd "$root"
${CXX:-clang++} -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Itests/nand_stub -Iplatform/include -Iplatform/src/mp tests/nand_safe_test.cpp platform/src/nand/nand.cpp platform/src/mp/mp_savefile.cpp platform/src/mp/mp_savecheck.cpp -pthread -o "$out/nand_safe"
"$out/nand_safe"

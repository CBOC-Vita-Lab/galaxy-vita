#!/usr/bin/env bash
# build the fixture: tools/build_few.sh <srcroot> <out> [extra flags...]
set -o pipefail   # (xcheck31: a failed compile piped into a filter must fail the script)
root=${1:?}; out=${2:?}; shift 2
cd "$root"
g++ -std=c++17 -g -O2 -Iplatform/vgx/src -Iplatform/vgx/third_party -pthread "$@" tests/vgx_fe_work.cpp platform/vgx/src/vgx_{state,draw,fesplit,dlcache,guestmem,bpsync,shadergen,texcache,texdecode,stats,backend_null,texcomp}.cpp -o "$out" 2>&1 | grep -E "error|Error" | head

#!/usr/bin/env bash
# GXV_INDCAP_VIS: the pure decision (tests/indcap_vis_test.cpp). Host g++ only.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
"${CXX:-g++}" -std=c++20 -O1 -Wall "$root/tests/indcap_vis_test.cpp" -o "$out/indcap_vis_test"
"$out/indcap_vis_test"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -Iplatform/include tests/depth_of_field.cpp -o local/settings-tests/depth_of_field
local/settings-tests/depth_of_field

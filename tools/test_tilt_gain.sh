#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Iplatform/include tests/tilt_gain.cpp -o local/settings-tests/tilt_gain
local/settings-tests/tilt_gain
for p in 50 63 75 88 120 140 160 180 200; do GXV_TILT_GAIN=$p local/settings-tests/tilt_gain --env $p; done
GXV_TILT_GAIN=130 local/settings-tests/tilt_gain --env 130
GXV_TILT_GAIN=bogus local/settings-tests/tilt_gain --env 100
GXV_TILT_GAIN=999 local/settings-tests/tilt_gain --env 200
env -u GXV_TILT_GAIN local/settings-tests/tilt_gain --env 100

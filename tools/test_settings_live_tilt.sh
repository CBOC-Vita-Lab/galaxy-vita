#!/usr/bin/env bash
# Host test: a Tilt change re-makes the hold-the-controller pictures. Run under the queue: tools/buildrun.sh bash tools/test_settings_live_tilt.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p local/settings-tests
${CXX:-clang++} -std=c++17 -Wall -Wextra -Werror -g -O1 -Iplatform/include -Itests platform/src/port/settings_live.cpp tests/settings_live_tilt.cpp -o local/settings-tests/settings_live_tilt
local/settings-tests/settings_live_tilt

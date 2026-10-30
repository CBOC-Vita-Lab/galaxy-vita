#!/usr/bin/env bash
# The picture-rate logic on a CPU-bound stretch, as a model (tests/pace_cpubound_model.cpp; not a console measurement).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
"${CXX:-g++}" -std=c++17 -O1 -I"$root/platform/include" "$root/tests/pace_cpubound_model.cpp" -o "$out/pace_cpubound_model"
"$out/pace_cpubound_model"

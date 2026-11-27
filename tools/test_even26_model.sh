#!/usr/bin/env bash
# pace26 host model (tests/even26_model.cpp, trace rows in tests/even26_trace.inc from the c79 console logs): the pacing policies side by side
# (judder, missed moments, latency, fps, share of pictures on the ideal cadence) plus the Even26 / EvenMeter unit checks. Pass -q for the quick run.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-g++}" -std=gnu++17 -O2 -I"$root/platform/include" "$root/tests/even26_model.cpp" -o "$out/m"
"$out/m" "$@"

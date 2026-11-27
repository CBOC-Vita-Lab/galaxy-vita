#!/usr/bin/env bash
# disc26: the pure play-time disc policies (port/io_policy.h). Host clang++.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${TMPDIR:-/tmp}/io_policy_$$
clang++ -std=c++20 -O1 -g -Wall -Wextra tests/io_policy_test.cpp -o "$out"
"$out"
rm -f "$out"

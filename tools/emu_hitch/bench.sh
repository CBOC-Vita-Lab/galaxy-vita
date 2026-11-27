#!/bin/sh
# Host-only, no user profiles, game assets or GPU required.
set -eu
case ${1:-full} in smoke|full) ;; *) echo 'usage: sh bench.sh [smoke|full]' >&2; exit 2 ;; esac
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/checkpoint-bench.XXXXXXXX")
trap 'rm -rf "$scratch"' EXIT
"${CXX:-c++}" -std=c++17 -pthread -O2 -Wall -Wextra -Werror "$here/bench_checkpoint.cpp" -o "$scratch/bench"
"$scratch/bench" "$scratch" "${1:-full}"

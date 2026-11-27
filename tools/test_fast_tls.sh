#!/usr/bin/env bash
# tls40: host test of vita/vgx-game/fast_tls_walk.h (the key-list walk of the fast pthread_getspecific path).
# fix47: also the OS key measured from the list (a pthread_key_t is a handle, not the key) and a model of the second-thread check.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/fast-tls.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
${CXX:-g++} -std=c++17 -O1 -Wall -Werror "$root/tests/fast_tls_walk.cpp" -o "$tmp/t"
"$tmp/t"

#!/usr/bin/env bash
# strk1118: host proof that fclose under fd_guard.cpp's lock cannot release a FILE slot another thread is claiming (a model of newlib's slot table,
# tests/stdio31/fclose_lock.cpp, linked with the REAL wrappers). "unlocked" (GXV_FCLOSE_LOCK=0) is the control and must show damage.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d /tmp/fclose_lock.XXXXXX); trap 'rm -rf "$out"' EXIT
nice -n 10 g++ -std=c++20 -O1 -g -pthread -w -o "$out/t" "$root/tests/stdio31/fclose_lock.cpp" "$root/vita/vgx-game/fd_guard.cpp" -I"$root/tests/stdio31/hostshim" 2>&1 | head -20
for i in 1 2 3; do "$out/t" locked; done
for i in 1 2 3; do "$out/t" unlocked; done
echo "fclose lock host proof: PASS"

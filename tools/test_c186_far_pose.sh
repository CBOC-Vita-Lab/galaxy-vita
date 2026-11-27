#!/usr/bin/env bash
# Compile the actual PbAnimSkips body against controllable stage and actor inputs.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -n '/^bool PbAnimSkips(LiveActor\* pActor) {$/,/^}$/p' decomp/src/Game/LiveActor/LiveActor.cpp > "$tmp/skip.inc"
test -s "$tmp/skip.inc"
"${CXX:-c++}" -std=c++17 -I"$tmp" -Iplatform/include tests/c186_far_pose.cpp -o "$tmp/test"
env -u GXV_DIAG_FULL_FAR_POSE "$tmp/test" off
GXV_DIAG_FULL_FAR_POSE=0 "$tmp/test" off
GXV_DIAG_FULL_FAR_POSE=10 "$tmp/test" off
GXV_DIAG_FULL_FAR_POSE=1 "$tmp/test" on

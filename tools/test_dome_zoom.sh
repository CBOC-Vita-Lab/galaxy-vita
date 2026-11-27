#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$root/tools/prepare_dome_zoom_fixture.py" "$out"
source "$root/tools/host_san.sh"
san=($(host_san "${CXX:-clang++}" -fsanitize=address,undefined))
"${CXX:-clang++}" -std=c++17 -O1 -g ${san[@]+"${san[@]}"} -DTARGET_PC -I"$out" \
  "$root/tests/dome_zoom.cpp" -o "$out/test"
if [[ ${1:-} == --expect-guard ]]; then
  shift
  GXV_ASTRO_DOME_ZOOM_GUARD=1 "$out/test" on "$@"
else
  env -u GXV_ASTRO_DOME_ZOOM_GUARD "$out/test" off "$@"
  GXV_ASTRO_DOME_ZOOM_GUARD=0 "$out/test" off "$@"
  GXV_ASTRO_DOME_ZOOM_GUARD=1 "$out/test" on "$@"
  GXV_ASTRO_DOME_ZOOM_GUARD=2 "$out/test" off "$@"
fi
"${CXX:-clang++}" -std=c++17 -O1 -g ${san[@]+"${san[@]}"} -I"$out" \
  "$root/tests/dome_zoom_settings.cpp" -o "$out/settings"
"$out/settings"

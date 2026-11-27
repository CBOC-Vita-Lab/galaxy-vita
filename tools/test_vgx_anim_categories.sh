#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
unset GXV_ANIM_FAMILIES
unset GXV_ANIM_MATERIAL
common=(-std=c++20 -O1 -g -Wall -Wextra -DTARGET_PC=1 -DPORT_PLATFORM_CODE=1 -DPORT_RUNTIME_DIAGNOSTICS=0
  -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/platform/include" -pthread)
"${CXX:-clang++}" "${common[@]}" -D__vita__ -DPORT_LOGGING=1 "$root/tests/vgx_anim_categories_test.cpp" -o "$out/categories"
for mode in off workload-only anim-only; do
  work=0; anim=0
  if test "$mode" = workload-only; then work=1; fi
  if test "$mode" = anim-only; then anim=1; fi
  GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$anim "$out/categories" "$mode"
done
for mode in timing malformed overflow labels classification concurrent; do
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_STRIDE=1 "$out/categories" "$mode"
done
env -u GXV_ANIM_STRIDE GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 "$out/categories" sampling
GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_STRIDE=1 "$out/categories" family-timing
GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_STRIDE=1 "$out/categories" family-malformed
GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_STRIDE=1 "$out/categories" family-rounding
GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_STRIDE=1 "$out/categories" family-concurrent
env -u GXV_ANIM_STRIDE GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 "$out/categories" family-sampling
for settings in '1 1 0' '1 0 1' '0 1 1'; do
  read -r work categories families <<< "$settings"
  GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$categories GXV_ANIM_FAMILIES=$families "$out/categories" family-disabled
done
for mode in material-timing material-nested material-malformed material-rounding material-concurrent material-discard; do
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_MATERIAL=1 GXV_ANIM_STRIDE=1 "$out/categories" "$mode"
done
env -u GXV_ANIM_STRIDE GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_MATERIAL=1 "$out/categories" material-sampling
env -u GXV_ANIM_MATERIAL GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 "$out/categories" material-disabled
for settings in '1 1 1 0' '1 1 0 1' '1 0 1 1' '0 1 1 1'; do
  read -r work categories families material <<< "$settings"
  GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$categories GXV_ANIM_FAMILIES=$families GXV_ANIM_MATERIAL=$material "$out/categories" material-disabled
done
for mode in nonvita vita-silent; do
  defs=(-DPORT_LOGGING=1)
  if test "$mode" = vita-silent; then defs=(-D__vita__ -DPORT_LOGGING=0); fi
  "${CXX:-clang++}" "${common[@]}" "${defs[@]}" "$root/tests/vgx_anim_categories_test.cpp" -o "$out/$mode"
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 "$out/$mode" "$mode"
  nm "$out/$mode" > "$out/$mode-symbols.txt"
  if grep -q port_workload_anim "$out/$mode-symbols.txt"; then
    echo 'FAIL: category wrappers must be absent outside logging-enabled Vita' >&2; exit 1
  fi
done
if test "$(uname -s)" = Linux; then
  "${CXX:-clang++}" "${common[@]}" -D__vita__ -DPORT_LOGGING=1 -DWORKLOAD_TEST_EMUTLS=1 -femulated-tls \
    "$root/tests/vgx_anim_categories_test.cpp" -Wl,--wrap=__emutls_get_address -o "$out/categories-emutls"
  for mode in off workload-only anim-only; do
    work=0; anim=0
    if test "$mode" = workload-only; then work=1; fi
    if test "$mode" = anim-only; then anim=1; fi
    GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$anim "$out/categories-emutls" "$mode"
  done
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_STRIDE=1 "$out/categories-emutls" malformed
  for settings in '1 1 0' '1 0 1' '0 1 1'; do
    read -r work categories families <<< "$settings"
    GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$categories GXV_ANIM_FAMILIES=$families "$out/categories-emutls" family-disabled
  done
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_STRIDE=1 "$out/categories-emutls" family-malformed
  for settings in '1 1 1 0' '1 1 0 1' '1 0 1 1' '0 1 1 1'; do
    read -r work categories families material <<< "$settings"
    GXV_WORKLOAD_PROBE=$work GXV_ANIM_CATEGORIES=$categories GXV_ANIM_FAMILIES=$families GXV_ANIM_MATERIAL=$material "$out/categories-emutls" material-disabled
  done
  GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 GXV_ANIM_MATERIAL=1 GXV_ANIM_STRIDE=1 "$out/categories-emutls" material-malformed
  env -u GXV_ANIM_MATERIAL GXV_WORKLOAD_PROBE=1 GXV_ANIM_CATEGORIES=1 GXV_ANIM_FAMILIES=1 "$out/categories-emutls" material-disabled
fi

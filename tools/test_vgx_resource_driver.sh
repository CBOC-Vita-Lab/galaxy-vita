#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
node - "$root" "$out" <<'NODE'
const fs = require('node:fs');
const [root, out] = process.argv.slice(2);
const source = fs.readFileSync(root + '/platform/vgx/vita/vgx_host_galaxy_vita.cpp', 'utf8');
const marker = 'extern "C" int msc_vgl_printf(const char* format, ...) {';
const begin = source.indexOf(marker), end = source.indexOf('\n}\n', begin);
if (begin < 0 || end < begin || source.indexOf(marker, begin + 1) !== -1) throw new Error('logger extraction shape changed');
fs.writeFileSync(out + '/vgx_driver_logger.inc', source.slice(begin, end + 3));
NODE
"${CXX:-clang++}" -std=c++17 -O1 -g -D__VGX_VITA__ -DVGX_NO_NEON -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=1 \
  -I"$root/tests/autoq_clock_stubs" -I"$out" -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/vgx_resource_driver.cpp" -o "$out/resource-driver"
for value in 0 1; do
  GXV_RESOURCE_AUDIT=$value UBSAN_OPTIONS=halt_on_error=1 "$out/resource-driver" "$value"
done

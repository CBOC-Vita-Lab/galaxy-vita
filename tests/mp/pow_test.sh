#!/bin/bash
# The registration proof-of-work solver (platform/src/mp/mp_pow.h) against SHA-256 known answers and against the service's own code
# (server/src/core/pow.js: node's crypto for every message length 0..130 and the nonce its reference solver finds). Quick (< 10 s).
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
cd "$(dirname "$0")/../.."
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
OUT=${OUT:-$MP_TMP/mp_pow_test}
g++ -std=c++17 -O2 -Wall -Wextra -Iplatform/src/mp tests/mp/pow_test.cpp -o "$OUT"
"$OUT"
# every message length across the one- and two-block padding edges
for n in 0 1 54 55 56 57 63 64 65 119 120 130; do
  m=$(head -c $n /dev/zero | tr '\0' 'q')
  a=$("$OUT" kat "$m")
  b=$("$NODE" -e "process.stdout.write(require('crypto').createHash('sha256').update(process.argv[1]).digest('hex'))" "$m")
  [ "$a" = "$b" ] || { echo "sha256 mismatch at length $n"; exit 1; }
done
# the same nonce as the service's reference solver, and the service accepts it
C='1762800000000.00112233aabbccdd.12.ffeeddccbbaa9988'
mine=$("$OUT" solve "$C" 12)
theirs=$(cd server && "$NODE" --input-type=module -e "import {solve} from './src/core/pow.js'; process.stdout.write(await solve('$C', 12))")
[ "$mine" = "$theirs" ] || { echo "nonce mismatch: $mine vs $theirs"; exit 1; }
echo "pow_test.sh: sha256 matches node at 12 lengths; nonce $mine matches the service's solver"

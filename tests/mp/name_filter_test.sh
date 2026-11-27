#!/bin/bash
# The game's name filter against the service's: the shared test names, and the data file is the one generated from the service's list.
set -eu
cd "$(dirname "$0")/../.."
NODE=${NODE:-$(command -v node || echo /opt/node22/bin/node)}
OUT=${OUT:-/tmp/mp_name_filter_test}
(cd server && "$NODE" scripts/gen-name-filter.mjs --check)
g++ -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined -Iplatform/src/mp tests/mp/name_filter_test.cpp -o "$OUT"
"$OUT" server/test/fixtures/name_vectors.txt

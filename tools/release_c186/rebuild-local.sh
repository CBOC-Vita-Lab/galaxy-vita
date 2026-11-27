#!/usr/bin/env bash
# Run from the source bundle's tools/release_c186 or alongside the delivered VPK.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if [[ -f "$here/../../vita/vgx-game/CMakeLists.txt" ]]; then
    tree=$(cd "$here/../.." && pwd)
else
    tree=$(cd "${C186_TREE:?Set C186_TREE to the source checkout when using a copied script}" && pwd)
fi
root=$(cd "$tree/.." && pwd)
out=${C186_OUT:-$root/outputs/galaxy-c186.2}
mkdir -p "$out"
out=$(cd "$out" && pwd)
kit=${C186_KIT:-$root/claude-galaxy/perf-handoff-1115/c181-build-kit-1115.tgz}
tls=${C186_TLS_INPUTS:-$root/claude-galaxy-3.0/xfer}
rev=$(git -C "$tree" rev-parse HEAD)
if ! git -C "$tree" diff --quiet HEAD --; then echo "Commit the source first; the compiler uses git archive HEAD." >&2; exit 1; fi
for f in "$kit" "$tls/"{mbedtls-068ff080-checkout,vdeps,vitasdk}.tgz; do test -r "$f"; done
image=sha256:f5d787d2029cc064b711ac253f3183a04aae121bce9dbd8fd3c708ae2b60d6f2
[[ $(docker image inspect codex-galaxy-native:20261007 --format '{{.Id}}') == "$image" ]] || { echo "Different compiler image" >&2; exit 1; }
docker run --rm --platform linux/amd64 --network none --pull never \
    -v "$out:/out" -v "$tree:/worktree:ro" -v "$here:/c186-scripts:ro" \
    -v "$kit:/in/kit.tgz:ro" -v "$tls/mbedtls-068ff080-checkout.tgz:/in/tls.tgz:ro" \
    -v "$tls/vdeps.tgz:/in/vdeps.tgz:ro" -v "$tls/vitasdk.tgz:/in/vitasdk.tgz:ro" \
    --entrypoint bash codex-galaxy-native:20261007 /c186-scripts/build-offline.sh "$rev" release
C186_OUT="$out" C186_TREE="$tree" python3 -B "$here/package.py" build
C186_OUT="$out" C186_TREE="$tree" python3 -B "$here/package.py" verify

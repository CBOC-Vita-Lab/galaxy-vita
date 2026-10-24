#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=${VITA_SOURCE_IMAGE:-codex-galaxy-native:20261007-arm64}
architecture=$(docker image inspect "$image" --format '{{.Architecture}}')
test -f "$root/build-vita-source/compile_commands.json"
docker run --rm --name "galaxy-vita-runtime-objects-$$" --platform "linux/$architecture" --cpus=2 \
  --mount "type=bind,source=$root,target=/source,readonly" \
  --mount "type=bind,source=$sdk,target=/opt/vitasdk,readonly" \
  --mount "type=bind,source=$root/build-vita-source,target=/build" \
  --workdir /build "$image" \
  python3 /source/vita/standalone_compile.py runtime "$@"

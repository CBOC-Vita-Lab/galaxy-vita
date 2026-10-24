#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=${VITA_BRINGUP_IMAGE:-codex-galaxy-native:20261007}
build="$root/build-vita-bringup"
test "$(docker image inspect "$image" --format '{{.Architecture}}')" = amd64
mkdir -p "$build"
git -C "$root" check-ignore -q build-vita-bringup/probe
mkdir "$build/.batch-lock" || { printf '%s\n' 'Bringup package already building.' >&2; exit 2; }
trap 'rmdir "$build/.batch-lock"' EXIT
image_id=$(docker image inspect "$image" --format '{{.Id}}')
printf '%s\n' "$image_id" > "$build/docker-image-id.txt"
git -C "$root" rev-parse HEAD > "$build/source-revision.txt"
shasum -a256 "$root/build-vita-runtime/libgalaxy_vita_runtime_partial.a" "$root/build-gxm-material/vita_shader_bundle.h" \
    "$root/vita/bringup/renderer.cpp" "$root/vita/bringup/main.cpp" > "$build/inputs-before.sha256"
docker run --rm --pull=never --platform linux/amd64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/source,readonly" \
    --mount "type=bind,source=$sdk,target=/opt/vitasdk,readonly" \
    --mount "type=bind,source=$build,target=/build" \
    -e VITASDK=/opt/vitasdk -w /build "$image_id" bash /source/vita/bringup/container-build.sh
shasum -a256 -c "$build/inputs-before.sha256" > "$build/inputs-after-verification.txt"

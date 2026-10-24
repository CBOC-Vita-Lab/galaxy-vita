#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=${VITA_GAME_IMAGE:-codex-galaxy-native:20261007}
build="$root/build-vita-game"
test "$(docker image inspect "$image" --format '{{.Architecture}}')" = amd64
mkdir -p "$build"
git -C "$root" check-ignore -q build-vita-game/probe
mkdir "$build/.batch-lock" || { printf '%s\n' 'Native game package already building.' >&2; exit 2; }
container="galaxy-vita-game-$$"
cleanup() {
    docker stop "$container" >/dev/null 2>&1 || true
    rmdir "$build/.batch-lock"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
image_id=$(docker image inspect "$image" --format '{{.Id}}')
printf '%s\n' "$image_id" > "$build/docker-image-id.txt"
git -C "$root" rev-parse HEAD > "$build/source-revision.txt"
git -C "$root" status --short > "$build/git-status-before.txt"
docker run --rm --name "$container" --pull=never --platform linux/amd64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/source,readonly" \
    --mount "type=bind,source=$sdk,target=/opt/vitasdk,readonly" \
    --mount "type=bind,source=$build,target=/build" \
    -e VITASDK=/opt/vitasdk -w /build "$image_id" bash /source/vita/game/container-build.sh

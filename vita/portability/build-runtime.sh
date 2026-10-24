#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
build="$root/build-vita-runtime"
sdk=${VITASDK:-/opt/vitasdk}
image=${VITA_RUNTIME_IMAGE:-codex-galaxy-native:20261007-arm64}
arguments=()
if [[ ${1:-full} == symbols ]]; then
  arguments=(--symbols-only)
elif [[ ${1:-full} == delta ]]; then
  arguments=(--delta)
elif [[ ${1:-full} != full ]]; then
  printf '%s\n' 'Use full, delta, or symbols.' >&2
  exit 2
fi
test -f "$root/build-vita-source/libgalaxy_vita_sources.a"
test "$(docker image inspect "$image" --format '{{.Architecture}}')" = arm64
image_id=$(docker image inspect "$image" --format '{{.Id}}')
mkdir -p "$build"
git -C "$root" check-ignore -q build-vita-runtime/probe
mkdir "$build/.batch-lock" || { printf '%s\n' 'Runtime audit already locked; do not duplicate it.' >&2; exit 2; }
container="galaxy-vita-portability-$$"
cleanup() {
  docker stop "$container" >/dev/null 2>&1 || true
  rmdir "$build/.batch-lock"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
git -C "$root" rev-parse HEAD > "$build/source-revision.txt"
git -C "$root" status --short > "$build/git-status-before.txt"
docker image inspect "$image_id" > "$build/docker-image.json"
printf 'VITA_RUNTIME_IMAGE=%q VITASDK=%q bash vita/portability/build-runtime.sh %q\n' "$image" "$sdk" "${1:-full}" > "$build/host-command-${1:-full}.txt"
command=(docker run --rm --name "$container" --platform linux/arm64 --cpus=2
  --mount "type=bind,source=$root,target=/source,readonly"
  --mount "type=bind,source=$sdk,target=/opt/vitasdk,readonly"
  --mount "type=bind,source=$root/build-vita-source,target=/gamebuild,readonly"
  --mount "type=bind,source=$build,target=/build"
  --workdir /build -e PYTHONDONTWRITEBYTECODE=1 -e "VITA_RUNTIME_IMAGE_ID=$image_id" "$image_id"
  bash /source/vita/portability/container-runtime.sh "${arguments[@]}")
printf '%q ' "${command[@]}" > "$build/docker-command-${1:-full}.txt"
printf '\n' >> "$build/docker-command-${1:-full}.txt"
set +e
"${command[@]}" 2>&1 | tee "$build/container-${1:-full}.log"
status=${PIPESTATUS[0]}
set -e
printf '%s\n' "$status" > "$build/wrapper-exit-${1:-full}.txt"
exit "$status"

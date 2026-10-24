#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
image=${VITA_SOURCE_IMAGE:-codex-strikers-tests:20261006}
build="$root/build-vita-source"
mode=${1:-full}
jobs=${VITA_SOURCE_JOBS:-2}
[[ "$jobs" =~ ^[1-8]$ ]] || { echo 'VITA_SOURCE_JOBS must be 1-8' >&2; exit 2; }
case "$mode" in full|configure) ;; *) echo 'Usage: bash vita/build-source.sh [full|configure]' >&2; exit 2 ;; esac
test -d "$sdk/arm-vita-eabi/include"
git -C "$root" check-ignore -q build-vita-source/provenance.json
mkdir -p "$build"
if ! mkdir "$build/.build-lock" 2>/dev/null; then
  echo "Another sidecar build owns $build/.build-lock" >&2
  exit 2
fi
trap 'rmdir "$build/.build-lock"' EXIT
docker image inspect "$image" --format '{{json .}}' > "$build/docker-image.json"
architecture=$(docker image inspect "$image" --format '{{.Architecture}}')
case "$architecture" in amd64|arm64) ;; *) echo "Unsupported compiler host $architecture" >&2; exit 2 ;; esac
git -C "$root" rev-parse HEAD > "$build/source-revision.txt"
git -C "$root" status --short > "$build/git-status-before.txt"
printf 'VITA_SOURCE_IMAGE=%q VITA_SOURCE_JOBS=%q bash vita/build-source.sh %q\n' "$image" "$jobs" "$mode" > "$build/host-command.txt"

# No source/assets are copied; only the ignored output directory is writable.
docker run --rm --name "galaxy-vita-source-$$" --platform "linux/$architecture" --cpus="$jobs" \
  --mount "type=bind,source=$root,target=/source,readonly" \
  --mount "type=bind,source=$sdk,target=/opt/vitasdk,readonly" \
  --mount "type=bind,source=$build,target=/build" \
  --env VITASDK=/opt/vitasdk --env VITA_SOURCE_JOBS="$jobs" --workdir /build \
  "$image" bash /source/vita/container-build.sh "$mode" \
  2>&1 | tee "$build/container.log"

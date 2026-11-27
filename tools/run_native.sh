#!/usr/bin/env bash
set -euo pipefail

main() {
scriptDirectory=$(cd "$(dirname "$0")" && pwd)
script="$scriptDirectory/$(basename "$0")"
root=$(cd "$scriptDirectory/.." && pwd)
cd "$root"
seconds=${1:-45}
name=${2:-boot-$(date +%Y%m%d-%H%M%S)}
[[ "$seconds" =~ ^[1-9][0-9]*$ && "$name" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
platform=${GALAXY_NATIVE_PLATFORM:-linux/arm64}
image=${GALAXY_NATIVE_IMAGE:-codex-galaxy-native:20261007-arm64}
build=${GALAXY_NATIVE_BUILD:-build-native-arm64}
[[ "$build" == build-* && "$build" != */* ]] || exit 2
test -x "$build/game"
test -f local/cooked/sys/fst.bin
run=local/runs/$name
mkdir -p local/runs
mkdir "$run"
docker image inspect "$image" > "$run/docker-image.json"
git rev-parse HEAD > "$run/source-revision.txt"
git diff > "$run/source-changes.diff"
git status --short > "$run/source-status.txt"
cp "$build/game" "$run/game"
shasum -a 256 "$run/game" > "$run/executable-sha256.txt"
git ls-files -z --cached --others --exclude-standard | xargs -0 shasum -a 256 > "$run/workspace-source-sha256.txt"
printf 'Diagnostic only: not Vita gameplay or FPS.\n' > "$run/evidence-scope.txt"
printf '%s' "${PETARI_INPUT:-}" > "$run/input-script.txt"
printf '%s\n' 'glibc.malloc.mmap_threshold=0' > "$run/host-allocation-policy.txt"
printf '%s\n' "${PETARI_NATIVE_ADDRESSES:-0}" > "$run/native-address-mode.txt"
cp "$script" "$run/runner-script.sh"
seed=${PETARI_SEED_RUN:-}
if [[ -n "$seed" ]]; then
  [[ "$seed" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
  test -d "local/runs/$seed/saves"
  cp -R "local/runs/$seed/saves" "$run/saves"
  printf '%s\n' "$seed" > "$run/seed-save-run.txt"
  find "$run/saves" -type f -exec shasum -a 256 {} + > "$run/seed-save-sha256.txt"
fi
shaderSeed=${PETARI_SEED_SHADER_RUN:-}
if [[ -n "$shaderSeed" ]]; then
  [[ "$shaderSeed" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
  test -f "local/runs/$shaderSeed/shader-cache"
  cp "local/runs/$shaderSeed/shader-cache" "$run/shader-cache"
  printf '%s\n' "$shaderSeed" > "$run/seed-shader-run.txt"
  shasum -a 256 "$run/shader-cache" > "$run/seed-shader-sha256.txt"
fi

# Bound even a stalled renderer, and keep all saves/cache/audio in this run.
docker run --rm --init --platform "$platform" --network none \
  -v "$root:/work" -w /work \
  --env GLIBC_TUNABLES=glibc.malloc.mmap_threshold=0 \
  --env SDL_AUDIODRIVER=dummy --env EGL_PLATFORM=surfaceless \
  --env LIBGL_ALWAYS_SOFTWARE=1 --env PETARI_PERFLOG=1 --env PETARI_THREADS=1 \
  --env PETARI_INPUT="${PETARI_INPUT:-}" \
  --env PETARI_INPUT_FILE="/work/$run/input-script.txt" \
  --env PETARI_SHOT_MS="${PETARI_SHOT_MS:-2000}" \
  --env PETARI_CPU_XF="${PETARI_CPU_XF:-}" \
  --env PETARI_DRAW_SPANS="${PETARI_DRAW_SPANS:-}" \
  --env PETARI_PORTABLE_TEV="${PETARI_PORTABLE_TEV:-}" \
  --env PETARI_NATIVE_ADDRESSES="${PETARI_NATIVE_ADDRESSES:-0}" \
  --env PETARI_SHADER_CACHE="/work/$run/shader-cache" \
  --env PETARI_WAV="/work/$run/mixed-audio.wav" \
  "$image" timeout --kill-after=10s "$((seconds + 120))s" \
  "/work/$run/game" /work/local/cooked "/work/$run" "$seconds" \
  > "$run/console.log" 2>&1
}

# Load the complete body before a long run; later source edits cannot change
# commands that bash would otherwise read after the docker process exits.
main "$@"; exit $?

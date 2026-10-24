#!/usr/bin/env bash
# Isolated shader audit entry point. Never opens an emulator app or a GUI.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCAL="$ROOT/local/shacc-tools"
PY="$LOCAL/host-venv/bin/python"
CMAKE="$LOCAL/host-venv/bin/cmake"
SOURCE="$LOCAL/src/Vita3K-Shacc"
BUILD="$ROOT/build-shacc-tools/native"

usage() {
    printf '%s\n' \
      'Usage: bash tools/build_shacc_host.sh status' \
      '       bash tools/build_shacc_host.sh build' \
      '       bash tools/build_shacc_host.sh compile SOURCE --stage vertex|fragment --out build-shacc-tools/NAME.gxp [--optimization 0..3] [--instruction-limit N]' \
      '       bash tools/build_shacc_host.sh accept [--label NAME] [--optimization 0..3] [--instruction-limit N]' \
      '       bash tools/build_shacc_host.sh reflect GXP' \
      '       bash tools/build_shacc_host.sh acceptance-status' \
      'Prepared pinned dependencies must exist in local/shacc-tools; see docs/vita-shader-toolchain.md.'
}

cd "$ROOT"
export HOME="$LOCAL/home" TMPDIR="$LOCAL/tmp"
export XDG_CONFIG_HOME="$HOME/config" XDG_CACHE_HOME="$HOME/cache" XDG_DATA_HOME="$HOME/data"
export PYTHONDONTWRITEBYTECODE=1 CMAKE_BUILD_PARALLEL_LEVEL=2
export PYTHONNOUSERSITE=1 PIP_CONFIG_FILE=/dev/null PIP_DISABLE_PIP_VERSION_CHECK=1
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
LOGSTAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
MODE="${1:-status}"
if (( $# )); then shift; fi
if [[ "$MODE" == --help || "$MODE" == -h ]]; then usage; exit 0; fi

if [[ ! -x "$PY" || ! -f "$LOCAL/compile_shader.py" ]]; then
    printf '%s\n' 'Missing prepared isolated toolchain; see docs/vita-shader-toolchain.md.' >&2
    exit 2
fi
mkdir -p "$LOCAL/logs" "$LOCAL/home" "$LOCAL/tmp"
case "$MODE" in
    status)
        "$PY" --version
        printf 'Shacc source: '
        git -C "$SOURCE" rev-parse HEAD
        printf 'Oracle source: '
        git -C "$LOCAL/src/open-shacccg" rev-parse HEAD
        file "$BUILD/bin/vita3k-shacc"
        printf '%s\n' 'Use compile/accept for real Sony compiler execution; native --setup is not provisioned.'
        ;;
    build)
        [[ "$(git -C "$SOURCE" rev-parse HEAD)" == cec711d4bcb1cb77279d186e5919f1b88e2fe1ab ]] || {
            printf '%s\n' 'Unexpected Shacc revision; refusing build.' >&2; exit 2;
        }
        "$PY" "$LOCAL/bounded.py" --seconds 240 --log "$LOCAL/logs/script-configure-$LOGSTAMP.log" -- \
          "$CMAKE" -S "$SOURCE" -B "$BUILD" -G Ninja \
          -DCMAKE_MAKE_PROGRAM="$LOCAL/host-venv/bin/ninja" \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DVITA3K_SHACC_LTO=OFF -DVITA3K_SHACC_FORCE_BUNDLED_BOOST=ON \
          -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON
        "$PY" "$LOCAL/bounded.py" --seconds 120 --log "$LOCAL/logs/script-build-$LOGSTAMP.log" -- \
          "$CMAKE" --build "$BUILD" --target vita3k-shacc --parallel 2
        ;;
    compile)
        exec "$PY" "$LOCAL/bounded.py" --seconds 90 --log "$LOCAL/logs/script-compile-$LOGSTAMP.log" -- \
          "$PY" "$LOCAL/compile_shader.py" "$@"
        ;;
    accept)
        exec "$PY" "$LOCAL/bounded.py" --seconds 600 --log "$LOCAL/logs/script-accept-$LOGSTAMP.log" -- \
          "$PY" "$LOCAL/accept_fixtures.py" "$@"
        ;;
    reflect)
        exec "$PY" "$LOCAL/src/open-shacccg/tools/gxp_info.py" "$@" --json
        ;;
    acceptance-status)
        exec "$PY" "$LOCAL/acceptance_status.py"
        ;;
    *) usage >&2; exit 2 ;;
esac

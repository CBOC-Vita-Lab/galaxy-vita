#!/usr/bin/env bash
# C186 release regression suite. No game files, emulator, console or internet required.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
mode=${1:-full}
if [[ $# -gt 1 || ! $mode =~ ^(full|core|integration|docker)$ ]]; then
    printf '%s\n' 'Usage: bash tools/test_c186.sh [full|core|integration|docker]' >&2
    exit 2
fi
if [[ $mode == docker || ( $mode == full && $(uname -s) == Darwin ) ]]; then
    archive=${C186_TLS_ARCHIVE:-"$root/../claude-galaxy-3.0/xfer/mbedtls-068ff080-checkout.tgz"}
    test -f "$archive" || { printf 'Set C186_TLS_ARCHIVE to the pinned third-party TLS checkout archive.\n' >&2; exit 2; }
    exec docker run --rm --pull never --platform linux/amd64 --network none \
        -v "$root:/src:ro" -v "$archive:/tmp/c186-archived-tls:ro" \
        -e C186_TLS_ARCHIVE=/tmp/c186-archived-tls -w /src --entrypoint bash \
        "${C186_HOST_IMAGE:-cboc-strikers-12-final-tests:20261031}" tools/test_c186.sh full
fi
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
export TMPDIR="$out" PYTHONDONTWRITEBYTECODE=1
export CXX=${CXX:-clang++} CC=${CC:-clang}
run() { printf '\n== Host: %s ==\n' "$*"; "$@"; }
if [[ $mode != integration ]]; then
    run python3 -B tests/test_c186_settings_link.py
    run python3 -B -m unittest discover -s tests -p 'test_c186_console*.py'
    run python3 -B -m unittest discover -s tests -p 'test_c186_hitch*.py'
    run sh tools/emu_hitch/test.sh
    run sh tools/emu_hitch/bench.sh smoke
    run python3 -I -B tools/emu_hitch/long_trace/verify.py
    run python3 -B tools/c186_emu_ab/replay.py --self-test
    run bash tools/test_hitch24.sh "$out/hitch24"
    run bash tools/test_hitch25.sh "$out/hitch25"
    run python3 -B tests/test_c186_retail_profile.py
    run bash tools/test_c186_far_pose.sh
    run bash tools/test_c184_archive.sh
    run node tools/test_c184_depth_plan.mjs
    run bash tools/test_j3d_envelope_neon.sh
    run bash tools/test_c183.sh
    run bash tools/test_menusplit.sh "$out/menu-split"
    run bash tools/test_vita_format.sh
fi
if [[ $mode != core ]]; then
    [[ $(uname -s) == Linux ]] || { echo 'Use docker for Linux/Mesa/TLS integration checks.' >&2; exit 2; }
    run bash tools/test_settings_native.sh "$out/settings"
    run bash tools/test_c182_firstvisit_backend.sh
    run bash tools/test_c182_warp_pixels.sh
    run env ASAN_OPTIONS=detect_leaks=0 bash tests/mp/ui_menu/build.sh "$out/mp-ui"
    run bash net/tests/run_online_tests.sh "$out/virtual-online"
    if [[ -z ${MBEDTLS:-} ]]; then
        : "${C186_TLS_ARCHIVE:?Set MBEDTLS to the pinned host64 install, or C186_TLS_ARCHIVE to the pinned checkout archive}"
        expected=40c4d7ce2610cba931007383fab8e3480d6922072b62fc9cfd1418651925934d
        printf '%s  %s\n' "$expected" "$C186_TLS_ARCHIVE" | sha256sum -c -
        mkdir -p "$out/deps/sources"
        tar --no-same-owner -xzf "$C186_TLS_ARCHIVE" -C "$out/deps/sources"
        run python3 -I tools/mp/build-mbedtls.py --target host64 --work "$out/deps"
        export MBEDTLS="$out/deps/mbedtls/host64"
    fi
    run bash net/tests/run_https_test.sh "$out/loopback-tls"
fi
printf '\nC186 host suite (%s): PASS. These are not console performance or gameplay measurements.\n' "$mode"

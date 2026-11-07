#!/usr/bin/env bash
# The packaged shader seed (platform/vgx/vita/vgx_host_galaxy_vita.cpp, SeedShaderCache / SeedShaderList and the read-only lookup
# behind vitaGL's cache, with vita/vgx-game/coldtrace_wrap.cpp's sceIoOpen wrapper) against an in-memory SceIo filesystem.
# Every scenario runs against the copy (GXV_VITA_SEED=copy, the behaviour before 1.2); those that do not depend on copying also run
# against the default, the seed read in place. tools/test_vita_seed_cache.sh [scenario]; NATIVE=1 (or no docker) builds with the
# host's clang++ instead of the arm64 image.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${VITASDK:-/opt/vitasdk}
vgl=${VGL_DEPS:-/opt/galaxy-vgl}
script='
    copy_modes="equal-size copy-short-read copy-read-error copy-close-error copy-short-write same changed dir-error cap cap-exact partial-list stamp-read-error stamp-close-error legacy marker-nul marker-short-write zero-gxp existing-gxp adopt-read-error adopt-write-error adopt-close-error bucket-early bucket-lengths list-stat-error adopt-stat-error adopt-missing adopt-retry adopt-crash-before-list adopt-large stamp-known stamp-chunks stamp-io-count list-override adopt-close-retry adopt-restore-fail adopt-marker-fail adopt-preread-close-error other-generator short-list other-generator-partial other-generator-seeded"
    lookup_modes="equal-size same stamp-read-error stamp-close-error marker-short-write adopt-read-error adopt-write-error adopt-close-error bucket-early bucket-lengths list-stat-error adopt-stat-error adopt-missing adopt-retry adopt-crash-before-list adopt-large stamp-known stamp-chunks stamp-io-count list-override adopt-close-retry adopt-restore-fail adopt-marker-fail adopt-preread-close-error lookup-first lookup-open lookup-switches lookup-legacy-seed lookup-no-version other-generator short-list other-generator-partial other-generator-seeded"
    export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1
    for probe in 0 1; do
    $CXX -std=c++20 $STDLIB -O1 -g -D__vita__ -D__VGX_VITA__ -DNOMINMAX -DVGX_COLDTRACE=$probe -fno-short-enums \
      -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=sceIoOpen \
      -Iplatform/include -Iplatform/vgx/vita/include -I$VGL/include -Iplatform/vgx/src \
      -Iplatform/vgx/third_party -idirafter $SDK/arm-vita-eabi/include \
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-asm-operand-widths \
      -fsanitize=address,undefined $SANLINK -fno-omit-frame-pointer \
      tests/vita_seed_cache.cpp vita/vgx-game/coldtrace_wrap.cpp -o $OUT/seed-cache $LIBS
    if test -n "$SEED_CASE"; then timeout 30s $OUT/seed-cache "$SEED_CASE"; GXV_VITA_SEED=copy timeout 30s $OUT/seed-cache "$SEED_CASE"; continue; fi
    for mode in $copy_modes; do GXV_VITA_SEED=copy timeout 30s $OUT/seed-cache "$mode"; done
    for mode in $lookup_modes; do timeout 30s $OUT/seed-cache "$mode"; done
    done
'
if test "${NATIVE:-0}" != 1 && docker info >/dev/null 2>&1; then
  docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" --mount "type=bind,source=$sdk,target=/sdk,readonly" \
    --mount "type=bind,source=$vgl,target=/vgl,readonly" -w /work -e "SEED_CASE=${1:-}" \
    -e CXX=clang++ -e STDLIB=-stdlib=libc++ -e SANLINK=-fno-sanitize-link-runtime -e "LIBS=-l:libasan.so.8 -l:libubsan.so.1" \
    -e VGL=/vgl -e SDK=/sdk -e OUT=/tmp \
    codex-galaxy-native:20261007-arm64 bash -euo pipefail -c "$script"
else
  out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
  cd "$root"
  SEED_CASE=${1:-} CXX=clang++ STDLIB= SANLINK= LIBS= VGL=$vgl SDK=$sdk OUT=$out bash -euo pipefail -c "$script"
fi

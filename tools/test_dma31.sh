#!/usr/bin/env bash
# dma31 (17 Nov 2026): host tests of the clock census (GXV_CLOCK_CENSUS, vita/vgx-game/clock_census.cpp) and of the back end's clock reads a
# picture, plus the Vita toolchain's syntax check of the changed files.
#   tools/test_dma31.sh [--before <git ref>] [out-dir]
#     1. tests/dma31_clock_census.cpp: the census with counting stubs for the system calls (ASan + UBSan)
#     1b. tests/dma31_dma_load.cpp: GXV_DMA_LOAD's copy (vgx_dma_load.h) with a stub DMA engine: every mode, verify catches a stale copy
#     2. tests/dma31_clock_backend.cpp: the real back end on the GL stub, built as the console builds it (__VGX_VITA__,
#        PORT_RUNTIME_DIAGNOSTICS=0), run with the console's defaults (entry.cpp's setenv(..., 0) lines): render-thread clock reads a picture,
#        with 250 draws and with 1 draw (no read may depend on the draw count). --before <ref> builds the same test against <ref>'s back end.
#     3. arm-vita-eabi syntax check of clock_census.cpp (gcc, as CMake builds it), vgx_backend_gl.cpp, vgx_draw.cpp and vgx_host_galaxy_vita.cpp (clang, the runtime's flags)
#     4. tests/test_player_audit.py (the front end's clock reads: vgx_draw.cpp now reads only in FeClock)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
before=""
if [[ ${1:-} == --before ]]; then before=$2; shift 2; fi
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/dma31.XXXXXX")}
mkdir -p "$out"
sdk=${VITASDK:-/opt/vitasdk}
cxx=${CXX:-clang++}

echo "== 1. clock census (host, counting stubs)"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
    -idirafter "$sdk/arm-vita-eabi/include" "$root/tests/dma31_clock_census.cpp" -o "$out/census" -pthread
"$out/census"

echo "== 1b. GXV_DMA_LOAD copy (host, stub engine)"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined "$root/tests/dma31_dma_load.cpp" -o "$out/dma_load"
"$out/dma_load"

echo "== 2. back end clock reads a picture (GL stub, console defaults)"
grep -o 'setenv("GXV_[A-Z0-9_]*", "[^"]*", 0)' "$root/vita/vgx-game/entry.cpp" | sed -E 's/setenv\("([^"]*)", "([^"]*)", 0\)/\1=\2/' |
    awk -F= '!seen[$1]++' > "$out/vita-defaults.env"
build_backend() { # <source root> <binary>
    local src=$1 bin=$2
    local flags=(-std=c++17 -O1 -DNDEBUG -w -D__VGX_VITA__=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -include "$root/tests/dma31_glshim.h" -I"$root/armhf/glstub"
                 -I"$root/tests/autoq_clock_stubs" -I"$src/platform/include" -I"$src/platform/vgx/src" -I"$src/platform/vgx/third_party")
    sed "s#\.\./platform/vgx/src/vgx_backend_gl.cpp#$src/platform/vgx/src/vgx_backend_gl.cpp#" "$root/tests/dma31_clock_backend.cpp" > "$bin.cpp"
    nice -n 10 "$cxx" "${flags[@]}" -I"$root/tests" "$bin.cpp" "$src/platform/vgx/src/vgx_shadergen.cpp" "$src/platform/vgx/src/vgx_autoq.cpp" -o "$bin" -lpthread
}
run_backend() { # <binary> <label>
    local r250 r1
    r250=$(env $(cat "$out/vita-defaults.env") GXV_LOG=0 "$1" 300 250 24 | grep '^clock-census-host:')
    r1=$(env $(cat "$out/vita-defaults.env") GXV_LOG=0 "$1" 300 1 24 | grep '^clock-census-host:')
    echo "  $2: $r250"
    echo "  $2: $r1"
    local a b
    a=$(sed -E 's/.*reads a picture ([0-9.]+) .*/\1/' <<<"$r250"); b=$(sed -E 's/.*reads a picture ([0-9.]+) .*/\1/' <<<"$r1")
    echo "$a" > "$out/$2.reads"
    [[ $a == "$b" ]] || { echo "FAIL: $2: the reads depend on the draw count ($a with 250 draws, $b with 1)"; exit 1; }
}
build_backend "$root" "$out/backend"
run_backend "$out/backend" after
after=$(cat "$out/after.reads")
awk -v a="$after" 'BEGIN { exit !(a + 0 <= 16.0) }' || { echo "FAIL: render-thread clock reads a picture $after > 16 (integ-3.1 34ee2865: 16; with the dma31 prologue merge: 13)"; exit 1; }
if [[ -n $before ]]; then
    mkdir -p "$out/before-src"
    git -C "$root" archive "$before" platform/vgx/src platform/vgx/third_party platform/include | tar -x -C "$out/before-src"
    build_backend "$out/before-src" "$out/backend-before"
    run_backend "$out/backend-before" before
    echo "  render-thread clock reads a picture: before $(cat "$out/before.reads"), after $after"
fi

echo "== 3. Vita toolchain syntax check"
"$sdk/bin/arm-vita-eabi-g++" -std=gnu++17 -O2 -Wall -Wextra -fno-short-enums -fno-exceptions -fno-rtti -fsyntax-only "$root/vita/vgx-game/clock_census.cpp"
echo "  clock_census.cpp: ok (arm-vita-eabi-g++)"
if [[ -f /vdeps/include/vitashark.h ]]; then
    "$sdk/bin/arm-vita-eabi-g++" -std=gnu++17 -O2 -Wall -fno-short-enums -fno-exceptions -fno-rtti -I/vdeps/include -fsyntax-only "$root/vita/vgx-game/wait_wrap.cpp"
    echo "  wait_wrap.cpp: ok (arm-vita-eabi-g++)"
fi
if command -v clang++ >/dev/null && [[ -f /vdeps/include/vitaGL.h ]]; then
    for f in src/vgx_backend_gl.cpp src/vgx_draw.cpp vita/vgx_host_galaxy_vita.cpp; do
    clang++ --target=arm-vita-eabi --sysroot="$sdk/arm-vita-eabi" -DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 '-DMP_BUILD_ID="test"' \
        -I"$root/vita/include" -I"$root/platform/include" -I"$root/decomp/include" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
        -fsigned-char -fno-short-enums -femulated-tls -fno-pic -D__vita__ -D__USES_INITFINI__ -nostdlibinc -nostdinc++ \
        -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0" -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/arm-vita-eabi" \
        -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/backward" -isystem "$sdk/arm-vita-eabi/include" -std=gnu++20 -fshort-wchar \
        -fno-exceptions -fno-rtti -w -DNOMINMAX -D__VGX_VITA__ -DGXV_ABSIDX_DEFAULT=0 -DGXV_STRICT_DRAW_PROFILE=1 -I"$root/platform/vgx/vita/include" \
        -I/vdeps/include -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/vgx/vita" -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -DPORT_LOGGING=1 -DGXV_SHADER_CACHE_PACK=1 '-DGXV_VGL_ID="test"' -fsyntax-only "$root/platform/vgx/$f"
    echo "  $f: ok (clang --target=arm-vita-eabi, the runtime's flags)"
    done
fi
echo "== 4. player audit (source guards)"
python3 -m pytest -q "$root/tests/test_player_audit.py" -k "not front_end_files" 2>&1 | tail -1
python3 - "$root" <<'PY'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(root / "tests"))
import test_player_audit as t
text = re.sub(r"//[^\n]*", "", (root / "platform/vgx/src/vgx_draw.cpp").read_text())
text = text.replace(t.function_body(text, "struct FeClock {"), "{}")
assert t.CLOCK_CALL.search(text) is None, "vgx_draw.cpp reads a clock outside FeClock"
print("  vgx_draw.cpp: no clock read outside FeClock")
PY
echo "PASS dma31"

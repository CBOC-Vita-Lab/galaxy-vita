#!/usr/bin/env bash
# texup31 (17 Nov 2026): host tests of the swizzled-upload switches' logic (platform/vgx/vita/vgx_texup_scratch.h: scratch pool, lazy-fence bookkeeping, verify,
# census line), the mutant check (each deliberate fault must make the test FAIL), the existing swizzle-layout test, and the Vita toolchain's syntax check.
#   tools/test_texup31.sh [out-dir]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/texup31.XXXXXX")}
mkdir -p "$out"
sdk=${VITASDK:-/opt/vitasdk}
hdr="$root/platform/vgx/vita/vgx_texup_scratch.h"

echo "== 1. scratch pool / verify / census (host, ASan + UBSan)"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/platform/vgx/vita" "$root/tests/texup31_scratch.cpp" -o "$out/t"
"$out/t"

echo "== 1b. swizzle layout (unchanged, still passes)"
g++ -std=c++17 -O1 -I"$root/platform/vgx/vita" "$root/tests/vgx_swizzle_layout.cpp" -o "$out/swz" && "$out/swz" | tail -1

echo "== 2. mutant check: each fault must be caught"
declare -a muts=(
 "s/if (b.st == kPending) Give(b);/if (false) Give(b);/|Flush leaves Pending blocks pending"
 "s/b.pooled \&\& b.st == kFree \&\& b.cap >= size/b.pooled \&\& b.st != kBusy \&\& b.cap >= size/|a Pending block is handed out again"
 "s/swz::SwizzledIndex(x, y, w, h)/(y * w + x)/|verify compares in linear order"
 "s/pendingBytes_ += b->cap;//|pending bytes not counted"
 "s/if (hooks_.free) hooks_.free(b.p);\n            b.p = nullptr;/b.p = nullptr;/|plain block not freed"
)
bad=0
for i in "${!muts[@]}"; do
    spec=${muts[$i]}; sedexpr=${spec%%|*}; name=${spec#*|}
    mkdir -p "$out/m$i"
    if [[ $i == 4 ]]; then
        python3 - "$hdr" "$out/m$i/vgx_texup_scratch.h" <<'PY'
import sys
s = open(sys.argv[1]).read()
old = "            if (hooks_.free) hooks_.free(b.p);\n            b.p = nullptr; // (removed by Compact)"
assert old in s
open(sys.argv[2], "w").write(s.replace(old, "            b.p = nullptr; // (removed by Compact)"))
PY
    else
        sed -e "$sedexpr" "$hdr" > "$out/m$i/vgx_texup_scratch.h"
        cmp -s "$hdr" "$out/m$i/vgx_texup_scratch.h" && { echo "  mutant $i ($name): the patch did not apply"; bad=1; continue; }
    fi
    cp "$root/platform/vgx/vita/vgx_swizzle_layout.h" "$out/m$i/"
    g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$out/m$i" "$root/tests/texup31_scratch.cpp" -o "$out/m$i/t" 2>/dev/null
    if ASAN_OPTIONS=detect_leaks=0 "$out/m$i/t" >"$out/m$i/log" 2>&1; then echo "  mutant $i ($name): NOT CAUGHT"; bad=1; else echo "  mutant $i ($name): caught"; fi
done
[[ $bad == 0 ]] || { echo "FAIL: a mutant survived"; exit 1; }

echo "== 3. Vita toolchain syntax check (clang --target=arm-vita-eabi, the runtime's flags)"
if command -v clang++ >/dev/null && [[ -f /vdeps/include/vitaGL.h ]]; then
    for f in src/vgx_backend_gl.cpp vita/vgx_host_galaxy_vita.cpp; do
    clang++ --target=arm-vita-eabi --sysroot="$sdk/arm-vita-eabi" -DNDEBUG=1 -DTARGET_PC=1 -DVERSION=0 '-DMP_BUILD_ID="test"' \
        -I"$root/vita/include" -I"$root/platform/include" -I"$root/decomp/include" -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
        -fsigned-char -fno-short-enums -femulated-tls -fno-pic -D__vita__ -D__USES_INITFINI__ -nostdlibinc -nostdinc++ \
        -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0" -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/arm-vita-eabi" \
        -isystem "$sdk/arm-vita-eabi/include/c++/15.2.0/backward" -isystem "$sdk/arm-vita-eabi/include" -std=gnu++20 -fshort-wchar \
        -fno-exceptions -fno-rtti -w -DNOMINMAX -D__VGX_VITA__ -DGXV_ABSIDX_DEFAULT=0 -DGXV_STRICT_DRAW_PROFILE=1 -I"$root/platform/vgx/vita/include" \
        -I/vdeps/include -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" -I"$root/platform/vgx/vita" -DPORT_RUNTIME_DIAGNOSTICS=0 \
        -DPORT_LOGGING=1 -DGXV_SHADER_CACHE_PACK=1 '-DGXV_VGL_ID="test"' -fsyntax-only "$root/platform/vgx/$f"
    echo "  $f: ok"
    done
else
    echo "  (clang++ or /vdeps/include/vitaGL.h missing: skipped)"
fi
echo "PASS texup31"

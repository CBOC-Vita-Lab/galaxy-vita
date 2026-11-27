#!/usr/bin/env bash
# streamc31 (17 Nov): GXV_DYN_CENSUS bit 2, the stream-decode census (platform/vgx/src/vgx_deccen.h, vgx_state.cpp DecCenNote). Passes when
#   1. tests/vgx_deccen.cpp (known answers on crafted draws) passes, built with ASan + UBSan;
#   2. each mutant of vgx_deccen.h below makes (1) fail (a census rule the test would not notice missing is a test that proves nothing);
#   3. the fe-work fixture (tests/vgx_fe_work.cpp: J3D-shaped scenes, particles, immediate draws through the real front end) prints the same
#      command buffers with GXV_DYN_CENSUS=0 / 1 / 2 / 3 (the census changes nothing drawn), for three scenes and two seeds, and, with
#      REF_ROOT=<a tree without the change>, the same as that tree's fixture; the census lines appear and count draws;
#   4. an integration mutant (the census writes one byte into the vertices it holds) makes (3) fail (SKIP_FE_MUTANT=1 leaves it out: it
#      builds the front end a second time).
#   tools/test_vgx_deccen.sh [frames]     (CXX: host compiler, default g++; build in $MP_TMP/vgx_deccen)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/../tests/mp/tmproot.sh"
root=$(cd "$(dirname "$0")/.." && pwd)
frames=${1:-90}
out=$MP_TMP/vgx_deccen
mkdir -p "$out"
cxx=${CXX:-g++}
fail=0
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

# ---- 1. known answers
$cxx -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -I"$root/platform/vgx/src" -I"$root/platform/vgx/third_party" \
     "$root/tests/vgx_deccen.cpp" -o "$out/deccen"
if "$out/deccen" > "$out/deccen.txt" 2>&1; then echo "known answers: $(tail -1 "$out/deccen.txt")"; else echo "FAIL known answers:"; grep FAIL "$out/deccen.txt" | head; fail=1; fi

# ---- 2. mutants of the census
caught=0; missed=0
mutant() {   # name, old, new
    local d="$out/mut-$1"
    rm -rf "$d"; mkdir -p "$d"
    cp "$root/platform/vgx/src/vgx_deccen.h" "$d/"
    if ! python3 -I - "$d/vgx_deccen.h" "$2" "$3" <<'EOF'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if s.count(old) != 1: sys.exit("pattern found %d times" % s.count(old))
open(p, "w").write(s.replace(old, new))
EOF
    then echo "BROKEN mutant $1 (the line to change was not found)"; missed=$((missed + 1)); return; fi
    if ! $cxx -std=c++17 -O1 -w -I"$d" -I"$root/platform/vgx/third_party" "$root/tests/vgx_deccen.cpp" -o "$d/t" > "$d/build.log" 2>&1; then
        echo "BROKEN mutant $1 (does not build)"; missed=$((missed + 1)); return
    fi
    if "$d/t" > "$d/log" 2>&1; then echo "MISSED mutant $1 (see $d/log)"; missed=$((missed + 1)); else caught=$((caught + 1)); rm -rf "$d"; fi
}
mutant arrays-not-in-whole '                    arraysSame &= same;' ''
mutant stride-not-in-whole 'const bool same = ra.arrLen == a.arrLen && ra.stride == a.stride && ra.le == a.le' 'const bool same = ra.arrLen == a.arrLen && ra.le == a.le'
mutant order-not-in-attr 'const bool arrSame = ra.arrLen == a.arrLen && ra.stride == a.stride && ra.le == a.le &&' 'const bool arrSame = ra.arrLen == a.arrLen && ra.stride == a.stride &&'
mutant no-ordinal 'const uint32_t ord = sigCount_[d.sig]++;' 'const uint32_t ord = 0;'
mutant clock-kept-in 'return static_cast<double>(ns) > off ?' 'return false ?'
mutant no-repeat 'if (!curH_.insert(h).second)' 'if (false && !curH_.insert(h).second)'
mutant hash-without-arrays 'if (d.a[i].indexed) h = XXH3_64bits_withSeed' 'if (false) h = XXH3_64bits_withSeed'
mutant nonpos-ignores-others '                    else nonPosSame &= same;' ''
mutant no-swap '        std::swap(prev_, cur_);' ''
mutant no-draw-cap 'const bool fits = need <= cap_ / 8 &&' 'const bool fits ='
mutant indices-not-compared 'bool same = SliceSame(pb + r.streamOff, d.stream, d.count, d.streamSize, a.srcOff, a.srcSize);' 'bool same = true;'
mutant moved-not-counted 'if (arrSame && ra.base != a.arr) ++w_.arrMoved[k];' ''
mutant whole-without-stream 'const bool whole = streamSame && arraysSame;' 'const bool whole = arraysSame;'
echo "census mutants: $caught caught, $missed missed"
[ $missed -eq 0 ] || fail=1

# ---- 3. the front end with the census: the same command buffers
srcs=(tests/vgx_fe_work.cpp platform/vgx/src/vgx_state.cpp platform/vgx/src/vgx_draw.cpp platform/vgx/src/vgx_fesplit.cpp
      platform/vgx/src/vgx_dlcache.cpp platform/vgx/src/vgx_guestmem.cpp platform/vgx/src/vgx_bpsync.cpp platform/vgx/src/vgx_shadergen.cpp
      platform/vgx/src/vgx_texcache.cpp platform/vgx/src/vgx_texdecode.cpp platform/vgx/src/vgx_stats.cpp platform/vgx/src/vgx_backend_null.cpp platform/vgx/src/vgx_texcomp.cpp)
build_fixture() {   # tree, objects folder, binary
    local tree=$1 od=$2 bin=$3 f
    mkdir -p "$od"
    for f in "${srcs[@]}"; do
        printf '%s\n' "$cxx -std=c++17 -O2 -g -w -I$tree/platform/vgx/src -I$tree/platform/include -I$tree/platform/vgx/third_party -pthread -c $tree/$f -o $od/$(basename "$f" .cpp).o"
    done > "$od/cmds"
    xargs -P "${JOBS:-2}" -I{} sh -c '{}' < "$od/cmds"
    $cxx -pthread "$od"/*.o -o "$bin"
}
build_fixture "$root" "$out/fe" "$out/few"
if [ -n "${REF_ROOT:-}" ]; then build_fixture "$REF_ROOT" "$out/fe-ref" "$out/few-ref"; fi
scenes=("FEW_FX=40 FEW_JPA=50 FEW_BIG=8 FEW_SHADOWDYN=4 FEW_CONSOLE=1" "FEW_JPA=80 FEW_JPAVAR=1 FEW_FUR=4" "FEW_FX=60 FEW_SHADOWDYN=8")
fixture_same() {   # binary to check; prints differences, returns 1 on any
    local bin=$1 bad=0 scene seed dc
    for scene in "${scenes[@]}"; do
        for seed in 1 2; do
            env $scene "$out/few" "$frames" "$seed" > "$out/base.txt" 2> /dev/null
            if [ -n "${REF_ROOT:-}" ] && [ "$bin" = "$out/few" ]; then
                env $scene "$out/few-ref" "$frames" "$seed" > "$out/ref.txt" 2> /dev/null
                cmp -s "$out/base.txt" "$out/ref.txt" || { echo "  differs from REF_ROOT: [$scene] seed $seed"; bad=1; }
            fi
            for dc in 1 2 3; do
                env $scene GXV_DYN_CENSUS=$dc GXV_DYN_CENSUS_MS=0 "$bin" "$frames" "$seed" > "$out/run.txt" 2> /dev/null
                cmp -s "$out/base.txt" "$out/run.txt" || { echo "  differs: [$scene] seed $seed GXV_DYN_CENSUS=$dc"; bad=1; }
            done
        done
    done
    return $bad
}
if fixture_same "$out/few"; then echo "fe-work fixture: GXV_DYN_CENSUS=0/1/2/3 the same command buffers (${#scenes[@]} scenes x 2 seeds${REF_ROOT:+, and the REF_ROOT tree})"; else echo "FAIL fe-work fixture"; fail=1; fi
env ${scenes[0]} FEW_LOG=1 GXV_DYN_CENSUS=2 GXV_DYN_CENSUS_MS=0 "$out/few" "$frames" 1 > /dev/null 2> "$out/census.err"
lines=$(grep -c "dyn-census decode: [0-9]* pictures" "$out/census.err" || true)
last=$(grep "dyn-census decode: [0-9]* pictures" "$out/census.err" | tail -1)
draws=$(printf '%s\n' "$last" | sed -n 's/.*stream draws \([0-9.]*\) .*/\1/p')
matched=$(printf '%s\n' "$last" | sed -n 's/.*matched to the picture before \([0-9.]*\);.*/\1/p')
old=$(grep -c "dyn-census: [0-9]* pictures; per picture" "$out/census.err" || true)
env ${scenes[0]} FEW_LOG=1 GXV_DYN_CENSUS=1 GXV_DYN_CENSUS_MS=0 "$out/few" 4 1 > /dev/null 2> "$out/census1.err"
if [ "$(grep -c "dyn-census: [0-9]* pictures; per picture" "$out/census1.err" || true)" -lt 3 ] || grep -q "dyn-census decode" "$out/census1.err"; then
    echo "FAIL GXV_DYN_CENSUS=1: not the old line alone"; fail=1
fi
if [ "$lines" -lt $((frames - 2)) ] || ! awk -v d="${draws:-0}" -v m="${matched:-0}" 'BEGIN { exit !(d > 100 && m > 50) }' || [ "$old" != 0 ]; then
    echo "FAIL census lines: $lines lines, stream draws ${draws:-?}, matched ${matched:-?}, old dyn-census lines with =2: $old"; fail=1
else
    echo "census lines: $lines (stream draws ${draws} a picture, matched ${matched}; =2 prints no old line)"
fi

# ---- 4. integration mutant: a census that changes a decoded vertex must be seen by (3)
if [ "${SKIP_FE_MUTANT:-0}" != 1 ]; then
    m="$out/femut"
    rm -rf "$m"; mkdir -p "$m/platform/vgx" "$m/platform/include" "$m/tests"
    cp -r "$root/platform/vgx/src" "$root/platform/vgx/third_party" "$m/platform/vgx/"; cp -r "$root/platform/include/port" "$m/platform/include/"; cp "$root/tests/vgx_fe_work.cpp" "$m/tests/"
    python3 -I - "$m/platform/vgx/src/vgx_state.cpp" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "    hold.assign(out, out + bytes);\n"
assert s.count(old) == 1
open(p, "w").write(s.replace(old, old + "    if (bytes) const_cast<uint8_t*>(out)[bytes - 1] ^= 1;\n"))
EOF
    build_fixture "$m" "$m/o" "$m/few"
    if fixture_same "$m/few" > "$m/log"; then echo "MISSED integration mutant (the census changed vertices and the fixture saw nothing)"; fail=1
    else echo "integration mutant caught ($(grep -c differs "$m/log") runs differ)"; rm -rf "$m"; fi
fi
[ $fail -eq 0 ] && echo "test_vgx_deccen: PASS" || { echo "test_vgx_deccen: FAIL"; exit 1; }

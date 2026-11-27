#!/usr/bin/env bash
# The Bee Mushroom crash on the host (tests/dl_lazy_retarget.cpp): the real J3DPacket.cpp + GDBase.c with AddressSanitizer.
# (clang compiles the decomp headers; the AddressSanitizer runtime is GCC's libasan, as tools/test_gd_write.sh links it.)
# Cases: key (GXV_DL_LAZY_KEY: entry left out), guard (GXV_DL_GUARD: overflow refused), crash (both off: ASan must report it).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$(mktemp -d)}
CXX=${CXX:-clang++}
CC=${CC:-clang}
common=(-g -O1 -Wno-register -DTARGET_PC=1 -fno-strict-aliasing -w -fsanitize=address -fno-omit-frame-pointer -include stdint.h
        -include "$root/tests/glibc_fabs_shim.h" -I"$root/platform/include" -I"$root/decomp/libs/JSystem/include"
        -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/include")
"$CC" -x c -Wno-implicit-function-declaration "${common[@]}" -c "$root/decomp/src/RVL_SDK/gd/GDBase.c" -o "$out/gdbase.o"
"$CXX" -std=c++20 -fms-extensions -include cfloat "${common[@]}" -c "$root/decomp/src/JSystem/J3DGraphBase/J3DPacket.cpp" -o "$out/j3dpacket.o"
"$CXX" -std=c++20 -fms-extensions -include cfloat "${common[@]}" "$root/tests/dl_lazy_retarget.cpp" "$out/j3dpacket.o" "$out/gdbase.o" \
    -fsanitize=address ${ASAN_LINK:--fno-sanitize-link-runtime -l:libasan.so.8} -o "$out/dl_lazy_retarget"
ASAN_OPTIONS=detect_leaks=0 "$out/dl_lazy_retarget" key
ASAN_OPTIONS=detect_leaks=0 "$out/dl_lazy_retarget" guard
for c in same roundtrip relist; do  # beefix25: the cases the community fix (PR #34) also covers
    ASAN_OPTIONS=detect_leaks=0 "$out/dl_lazy_retarget" "$c"
done
for c in matrix matrixguard; do  # prog32a: every form change (six bodies: pairs, chains, the electric flicker)
    ASAN_OPTIONS=detect_leaks=0 "$out/dl_lazy_retarget" "$c"
done
if ASAN_OPTIONS=detect_leaks=0 "$out/dl_lazy_retarget" crash > "$out/crash.txt" 2>&1; then
    echo "dl_lazy_retarget crash: expected AddressSanitizer to stop it"; exit 1
fi
grep -q "heap-buffer-overflow" "$out/crash.txt" && echo "dl_lazy_retarget crash: AddressSanitizer: $(grep -m1 -o 'heap-buffer-overflow.*' "$out/crash.txt")"

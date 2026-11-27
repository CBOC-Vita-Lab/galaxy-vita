#!/usr/bin/env bash
# tests/gd_material_writers.cpp against the real decomp/src writers (J3DGD.cpp, GDBase.c), with ASan/UBSan.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
image=${GALAXY_TEST_IMAGE:-codex-galaxy-native:20261007-arm64}
docker run --rm --pull=never --platform linux/arm64 --network none --cpus=2 \
    --mount "type=bind,source=$root,target=/work,readonly" -w /work \
    --entrypoint bash "$image" -euo pipefail -c '
    flags=(-O2 -g -fms-extensions -DTARGET_PC=1 -fno-strict-aliasing -ffp-contract=on -fno-fast-math
        -Wno-deprecated-enum-enum-conversion -Wno-implicit-function-declaration
        -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer
        -Iplatform/include -Idecomp/libs/JSystem/include -Idecomp/libs/RVL_SDK/include -Idecomp/include)
    # GDBase.c keeps its __GDCurrentDL; its GDOverflowed gives way to the test'"'"'s callback.
    clang -std=c11 "${flags[@]}" -DGDOverflowed=gdbase_overflowed -c decomp/src/RVL_SDK/gd/GDBase.c -o /tmp/gdbase.o
    clang++ -std=c++20 "${flags[@]}" -c decomp/src/JSystem/J3DGraphBase/J3DGD.cpp -o /tmp/j3dgd.o
    clang++ -std=c++20 "${flags[@]}" tests/gd_material_writers.cpp /tmp/j3dgd.o /tmp/gdbase.o -Wl,--gc-sections \
        -Wl,--defsym=__PortGXFifoPtr=0 -Wl,--defsym=__PortGXFifoLimit=0 -Wl,--defsym=__PortGXFifoOverflow=0 \
        -Wl,--defsym=port_host_to_phys=0 -o /tmp/gd_material_writers
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/gd_material_writers
    '

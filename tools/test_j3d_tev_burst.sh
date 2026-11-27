#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ ${1:-} != --native ]]; then
    [[ $# = 0 ]] || { printf 'Usage: %s [--native]\n' "$0" >&2; exit 2; }
    docker run --rm --pull=never --platform linux/arm64 --network none --cpus=1 --memory=1g \
        --mount "type=bind,source=$root,target=/work,readonly" -w /work \
        sha256:b1818409343c92c116061814adc549148743e983153c39928cb97eb40a28e443 \
        bash tools/test_j3d_tev_burst.sh --native
    exit
fi
[[ $# = 1 ]] || exit 2
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
driver="$root/tests/j3d_tev_burst.cpp"
common=(-std=c++17 -O2 -fno-strict-aliasing -fno-fast-math -DTARGET_PC=1
    -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=all
    -I"$root/platform/include" -I"$root/decomp/libs/RVL_SDK/include" -I"$root/decomp/libs/JSystem/include")
case $(uname -s) in
    Darwin) gc=(-Wl,-dead_strip);;
    Linux) gc=(-Wl,--gc-sections -fno-sanitize-link-runtime -l:libasan.so.8 -l:libubsan.so.1);;
    *) printf 'Unsupported native host\n' >&2; exit 2;;
esac
cxx=${CXX:-clang++}
# Compile the whole production TU; only the two tested symbols are renamed.
"$cxx" "${common[@]}" -DTEV_BURST_HEADER_ONLY=1 -include "$driver" \
    -DJ3DGDSetTevKColor=Burst_J3DGDSetTevKColor \
    -DJ3DGDSetTevColorS10=Burst_J3DGDSetTevColorS10 \
    -c "$root/decomp/src/JSystem/J3DGraphBase/J3DGD.cpp" -o "$out/actual.o"
"$cxx" "${common[@]}" "$driver" "$out/actual.o" "${gc[@]}" -o "$out/proof"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out/proof"

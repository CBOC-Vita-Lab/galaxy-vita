#!/usr/bin/env bash
# Host llvmpipe code generation only. These are not Vita GPU instructions.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d)}
mkdir -p "$out"
probe=(copy-probe)
if [[ $# == 3 ]]; then probe=(key-probe "$2" "$3"); fi
for mode in 0 1; do
  GXV_TEVWRAP_ELIDE="$mode" LP_DEBUG=fs GALLIVM_DEBUG=asm \
    bash "$root/tools/test_vgx_tevwrap_gl.sh" "$out/build" "${probe[@]}" > "$out/mode-$mode.log" 2>&1
done
python3 - "$out" <<'PY'
import json
import pathlib
import re
import sys

results = []
for mode in (0, 1):
    text = (pathlib.Path(sys.argv[1]) / f"mode-{mode}.log").read_text()
    renderer = re.search(r"^GL renderer: (.+)$", text, re.M)
    pixel = re.search(r"^PASS: compiler probe pixel digest (\w+)$", text, re.M)
    assert renderer and "llvmpipe" in renderer[1] and pixel, "expected successful llvmpipe probe"
    result = {"mode": mode, "renderer": renderer[1], "pixel_digest": pixel[1],
              "nir_floor": len(re.findall(r"= ffloor ", text)), "assembly": {}}
    for name in ("fs_variant_partial", "fs_variant_whole"):
        marker = name + ":\n"
        assert text.count(marker) == 1, "expected exactly one fragment variant"
        block = text.split(marker, 1)[1].split("\n\n", 1)[0]
        ops = re.findall(r"^\s*[0-9a-f]+:\s+(\w+)", block, re.M)
        assert ops and ops[-1] == "ret", "expected complete AArch64 fragment disassembly"
        result["assembly"][name] = {"instructions": len(ops), "frintm": ops.count("frintm")}
    results.append(result)
print(json.dumps({"host_only": True, "results": results}, indent=2), flush=True)
assert results[0]["pixel_digest"] == results[1]["pixel_digest"]
assert results[0]["nir_floor"] > results[1]["nir_floor"]
for name in results[0]["assembly"]:
    before, after = (r["assembly"][name] for r in results)
    assert before["frintm"] > after["frintm"]
    assert before["instructions"] > after["instructions"]
print("PASS: host optimized NIR and final CPU assembly lose redundant floor work")
PY

"""Validate the actual compact seed writer and target C++ reader, including a corrupt package."""
import os
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from c182_compact_seed import compact

ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) > 1:
    with ZipFile(sys.argv[1]) as z:
        files = {n: z.read(n) for n in z.namelist() if n.startswith("shadercache/") and not n.endswith("/")}
else:
    pack = bytearray(40)
    pack[:8] = b"VGXRAM01"
    files = {"shadercache/b4d4c2a9/stages.pack": bytes(pack)}
    for i in range(1, 33):
        p = bytearray(160)
        p[4:8] = b"GXP\0"
        struct.pack_into("<I", p, 12, 156)
        struct.pack_into("<I", p, 24, i % 2)
        p[-1] = i
        files[f"shadercache/b4d4c2a9/v3/{'f' if i % 2 else 'v'}/00/{i:X}.gxp"] = bytes(p)
result = compact(files)
path, = (n for n in result if n.endswith("seed.c182"))
image = result[path]
_, _, count, abi, _, _ = struct.unpack_from("<8sIIQII", image)
originals = {n.split("/v3/")[1]: b for n, b in files.items() if n.endswith(".gxp") and n not in result}
assert len(originals) == count
with TemporaryDirectory() as tmp:
    d = Path(tmp)
    with (d / "reference").open("wb") as f:
        for n, b in originals.items():
            f.write(f"{n}\n{len(b)}\n".encode() + b)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread", "-O1", "-fsanitize=address,undefined",
                    "-Iplatform/vgx/vita", "-Iplatform/include", "-Iplatform/vgx/third_party",
                    "tests/c182_seed_read.cpp", "-o", d/"test"], check=True, cwd=ROOT)
    for case, offset in (("ok", None), ("bad-index", 35), ("bad-record", len(image) - 1)):
        data = bytearray(image)
        if offset is not None:
            data[offset] ^= 7
        (d / "pack").write_bytes(data)
        subprocess.run([d/"test", d/"pack", d/"reference", f"{abi:X}", case, str(count)], check=True)

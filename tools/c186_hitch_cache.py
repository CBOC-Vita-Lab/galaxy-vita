"""Prepare a warm Vita3K Vulkan cache for a fresh test profile. No Vita access."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct


TITLE_DIR = ("shaders", "SMGV00001", "eboot.bin")
PIPE = "pipeline-cache-vk15.dat"
INDEX = "hashs-vk.dat"
SPV = re.compile(r"vk15-[0-9a-f]{64}\.spv")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def inventory(path):
    if path.parts[-3:] != TITLE_DIR or path.absolute() != path.resolve(strict=True):
        raise ValueError("expected a real Vita3K Galaxy host cache, not a guest directory")
    files = {}
    for f in path.iterdir():
        if f.is_symlink() or not f.is_file() or not (f.name in (PIPE, INDEX) or SPV.fullmatch(f.name)):
            raise ValueError("unexpected host-cache entry: refusing to import")
        if f.stat().st_size <= 0:
            raise ValueError("empty host cache component")
        files[f.name] = f
    if PIPE not in files or INDEX not in files or len(files) < 3:
        raise ValueError("incomplete host cache")
    return files


def check_index(files):
    with files[INDEX].open("rb") as f:
        header = f.read(16)
        if len(header) != 16:
            raise ValueError("short Vita3K shader index")
        count, version, features = struct.unpack("<QII", header)
        if not count or version != 15 or os.fstat(f.fileno()).st_size != 16 + count * 64:
            raise ValueError("invalid Vita3K shader index length or version")
        for _ in range(count):
            for key in struct.unpack("32s32s", f.read(64)):
                if any(key) and f"vk15-{key.hex()}.spv" not in files:
                    raise ValueError("indexed shader missing from host cache")
    return features


def driver_id(path):
    # Vita3K 4232 on a little-endian 64-bit host: magic, size_t count,
    # then the pipeline keys, then VkPipelineCacheHeaderVersionOne.
    with path.open("rb") as f:
        header = f.read(12)
        if len(header) != 12:
            raise ValueError("short Vita3K cache header")
        magic, n = struct.unpack("<IQ", header)
        if magic != 0xbeef4321 or 12 + 8 * n + 32 >= os.fstat(f.fileno()).st_size:
            raise ValueError("invalid Vita3K cache size or magic")
        f.seek(8 * n, os.SEEK_CUR)
        size, version, vendor, device, uuid = struct.unpack("<IIII16s", f.read(32))
        if size != 32 or version != 1:
            raise ValueError("not a version-one Vulkan pipeline cache")
        return (vendor, device, uuid.hex())


def make_host_overlay(source_title_cache, baseline_title_cache, new_output_dir, mode="all"):
    """Stage independently written files. Caller must use closed, matched QA fixtures."""
    if mode != "all":
        raise ValueError("partial Vulkan caches are not supported; keep the index, SPIR-V and driver file together")
    source, base, out = map(Path, (source_title_cache, baseline_title_cache, new_output_dir))
    donor, seed = inventory(source), inventory(base)
    out = out.absolute()
    if (out.exists() or out.is_symlink() or out.parent != out.parent.resolve(strict=True)
            or any(p in out.parents or out in p.parents for p in (source.resolve(), base.resolve()))):
        raise ValueError("destination must be unused, outside both inputs, in a real directory")
    gpu = driver_id(donor[PIPE])
    if gpu != driver_id(seed[PIPE]):
        raise ValueError("Vulkan vendor, device or driver cache UUID differs")
    if check_index(donor) != check_index(seed):
        raise ValueError("Vita3K shader feature masks differ")
    if not seed.keys() <= donor.keys():
        raise ValueError("donor is missing files from the reference cache")
    for name in seed:
        if SPV.fullmatch(name) and digest(seed[name]) != digest(donor[name]):
            raise ValueError("same-named shader differs from reference; incompatible cache")
    files = donor
    # Reserve exclusively so a concurrent/colliding call cannot replace a destination.
    # The caller only activates this directory after the return; either input stays intact.
    out.mkdir()
    h = hashlib.sha256()
    try:
        for name, original in sorted(files.items()):
            dest = out / name
            copied_from = digest(original)
            shutil.copyfile(original, dest)
            if digest(dest) != copied_from:
                raise ValueError("cache changed while copying or readback failed")
            h.update(name.encode("ascii") + b"\0" + copied_from.encode("ascii") + b"\n")
        if driver_id(out / PIPE) != gpu:
            raise ValueError("driver cache readback changed")
    except BaseException:
        shutil.rmtree(out)  # only the directory created by this invocation
        raise
    return {"scope": "Vita3K host only", "mode": mode, "driver": gpu, "files": len(files),
            "tree_sha256": h.hexdigest(), "guest_cache_imported": False}

"""Pack verified loose seed binaries into one seekable file; never compile or change one."""
import re
import struct
from zlib import crc32

PATH = re.compile(r"shadercache/([0-9a-f]{8})/v3/([vf])/([0-9A-F]{2})/([0-9A-F]{1,16})\.gxp")
HEADER = struct.Struct("<8sIIQII")
ROW = struct.Struct("<IIQII")
# The reader's limits (platform/vgx/vita/vgx_compact_seed.h:35-45). One place: refill_pack.py's cap check imports these.
MAX_ENTRIES, MAX_BODY, MAX_RECORD = 8192, 8 << 20, 4096


def cap_report(sizes):
    """seedgate31 A2: what compact() would do with compiled files of these sizes (an iterable of byte counts): (entries, body bytes, loose count, problems).
    A file of 0 or over MAX_RECORD bytes stays loose (readable by the original loader, but not in the indexed seed): counted and reported, never silent.
    problems: one text per cap that is exceeded (entries over MAX_ENTRIES, body over MAX_BODY)."""
    sizes = list(sizes)
    kept = [n for n in sizes if 1 <= n <= MAX_RECORD]
    entries, body, loose = len(kept), sum(kept), len(sizes) - len(kept)
    problems = []
    if entries > MAX_ENTRIES: problems.append(f"compact seed: {entries} entries (limit {MAX_ENTRIES}): compact() would refuse it")
    if body > MAX_BODY: problems.append(f"compact seed: {body} body bytes (limit {MAX_BODY}): compact() would refuse it")
    if not entries: problems.append("compact seed: no entry (compact() would refuse it)")
    return entries, body, loose, problems


def compact(files, strict=False):
    """strict (seedgate31 A2): a file that stays loose (over MAX_RECORD bytes) raises instead of being left out of the indexed seed without a word"""
    pack_name, = (n for n in files if n.endswith("/stages.pack"))
    pack = files[pack_name]
    if pack[:8] != b"VGXRAM01":
        raise ValueError("missing validated RAM-pack identity")
    abi, = struct.unpack_from("<Q", pack, 24)
    records, result = [], dict(files)
    for name, payload in files.items():
        m = PATH.fullmatch(name)
        if not m:
            continue
        key, kind, bucket, token = m.groups()
        shader = int(token, 16)
        if f"shadercache/{key}/stages.pack" != pack_name or shader >> 56 != int(bucket, 16) or f"{shader:X}" != token:
            raise ValueError(f"noncanonical seed key: {name}")
        if not 1 <= len(payload) <= MAX_RECORD:
            if strict:
                raise ValueError(f"compact seed: {name} is {len(payload)} bytes (the indexed seed holds 1..{MAX_RECORD}); it would stay loose")
            continue  # oversized future shaders stay loose, readable by the original loader (strict=True: refused)
        records.append((1 if kind == "v" else 2, shader, name, payload))
    table, body, seen = bytearray(), bytearray(), set()
    for kind, shader, name, payload in sorted(records):
        if (kind, shader) in seen:
            raise ValueError("duplicate seed key")
        seen.add((kind, shader))
        table.extend(ROW.pack(kind, len(payload), shader, len(body), crc32(payload)))
        body.extend(payload)
        del result[name]
    if not 1 <= len(seen) <= MAX_ENTRIES or len(body) > MAX_BODY:
        raise ValueError("compact seed out of bounds")
    header = HEADER.pack(b"SMGSEED1", 1, len(seen), abi, len(body), crc32(table))
    result[pack_name.removesuffix("stages.pack") + "v3/seed.c182"] = header + table + body
    return result

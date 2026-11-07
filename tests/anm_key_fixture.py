"""Writes the fixtures of tests/anm_key_repair.cpp: one big-endian key animation (a .bck, ANK1) per case, converted
twice by tools/cook/j3d.py: as the older converter did (key counts read signed) and as it does now.

    anm_key_fixture.py OUTDIR      writes OUTDIR/<case>.old.bin and OUTDIR/<case>.new.bin
"""
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "cook"))
import j3d  # noqa: E402
from util import s16, u32, sw16, sw32, sw_range  # noqa: E402

PADDING = b"This is padding data to alig"

# case name: (scale, rotation, translation) key counts
CASES = {
    "small": (300, 2500, 700),
    "edge": (0x7FFF, 0x7FFF, 0x7FFF),
    "rot": (33, 40000, 900),
    "trans": (12, 2700, 39749),
    "scale": (32768, 2000, 150),
    "all": (33000, 63106, 40000),
}


def tracks(count, pack, rng, value):
    """`count` array entries as three equal tracks of (time, value, tangent) keys: returns (entries, [(keys, offset)])."""
    keys = count // 9
    entries = []
    spans = []
    for _c in range(3):
        spans.append((keys, len(entries)))
        for k in range(keys):
            entries += [k, value(rng), value(rng)]
    entries += [0] * (count - len(entries))
    return b"".join(pack(e) for e in entries), spans


def build_big_endian(ns, nr, nt, seed=7):
    rng = random.Random(seed)
    sc, sc_spans = tracks(ns, lambda e: struct.pack(">f", e), rng, lambda r: r.uniform(0.5, 2.0))
    ro, ro_spans = tracks(nr, lambda e: struct.pack(">h", e), rng, lambda r: r.randint(-30000, 30000))
    tr, tr_spans = tracks(nt, lambda e: struct.pack(">f", e), rng, lambda r: r.uniform(-500.0, 500.0))
    table = b""
    for c in range(3):
        for spans in (sc_spans, ro_spans, tr_spans):
            keys, off = spans[c]
            table += struct.pack(">HHH", keys, off, 0)
    tbl = 0x40
    sc_off = tbl + len(table)
    ro_off = sc_off + len(sc)
    tr_off = (ro_off + len(ro) + 3) & ~3
    end = tr_off + len(tr)
    block = b"ANK1" + struct.pack(">I", end)
    block += struct.pack(">BbhHHHH", 0, 1, 2000, 1, ns, nr, nt)
    block += struct.pack(">IIII", tbl, sc_off, ro_off, tr_off)
    block += PADDING
    assert len(block) == tbl
    block += table + sc + ro + b"\0" * (tr_off - ro_off - len(ro)) + tr
    head = b"J3D1bck1" + struct.pack(">II", 0x20 + len(block), 1) + b"SVR3" + b"\xff" * 8 + struct.pack(">I", 0xFFFFFFFF)
    return bytearray(head + block)


def ank1_signed(b, o, end):
    """tools/cook/j3d.py ank1 as it was before the unsigned counts: the three key counts read as signed 16-bit numbers."""
    n_s, n_r, n_t = s16(b, o + 0x0E), s16(b, o + 0x10), s16(b, o + 0x12)
    tbl, sc, rot, tr = [u32(b, o + 0x14 + 4 * i) for i in range(4)]
    if tbl:
        sw_range(b, o + tbl, (o + sc) if sc else end, 2)
    if sc:
        sw32(b, o + sc, n_s)
    if rot:
        sw16(b, o + rot, n_r)
    if tr:
        sw32(b, o + tr, n_t)
    sw16(b, o + 0x0A, 5)
    sw32(b, o + 0x14, 4)


def convert(be, legacy):
    b = bytearray(be)
    saved = j3d.ANIM_BLOCKS[b"ANK1"]
    if legacy:
        j3d.ANIM_BLOCKS[b"ANK1"] = ank1_signed
    try:
        j3d.swap_anim(b)
    finally:
        j3d.ANIM_BLOCKS[b"ANK1"] = saved
    return bytes(b)


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for name, (ns, nr, nt) in CASES.items():
        be = build_big_endian(ns, nr, nt)
        (out / (name + ".old.bin")).write_bytes(convert(be, True))
        (out / (name + ".new.bin")).write_bytes(convert(be, False))
        (out / (name + ".counts")).write_text("%d %d %d\n" % (ns, nr, nt))


if __name__ == "__main__":
    main(sys.argv[1])

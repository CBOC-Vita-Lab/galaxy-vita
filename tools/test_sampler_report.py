#!/usr/bin/env python3
"""test_sampler_report.py: builds a synthetic sampler-<n>.bin (the layout of platform/include/port/sampler_hist.h) with a fake nm listing and checks that
tools/sampler_report.py maps it. usage: python3 -I tools/test_sampler_report.py [workdir]"""
import os, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
work = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
os.makedirs(work, exist_ok=True)
nm = os.path.join(work, "nm.txt")
open(nm, "w").write("""81000000 00000100 T port_sampler_boot
81001000 00001000 T LiveActor::calcAnim()
81002000 00001000 T MarioActor::movement()
81003000 00001000 T MR::getFrameLength(int)
81004000 00001000 T vgx::Submit(int)
81005000 00001000 T memcpy
81006000 00001000 T SomethingUnknown()
""")
def slot(uid, name, stops, hold, hmax, fails):
    return struct.pack("<6I24s", uid, 1, stops, hold, hmax, fails, name.encode())
def build(path, index, load):          # load: the run-time address minus the ELF address
    ents = [(0x81001010 + load, 0x81002020 + load, 0, 60), (0x81002040 + load, 0x81000000 + 0x4000 + load, 0, 30), (0x81003004 + load, 0x81001100 + load, 0, 25),
            (0x81004100 + load, 0x81001100 + load, 0, 10), (0x81005000 + load, 0x81001100 + load, 0, 5), (0x81006000 + load, 0, 0, 3),
            (0x40010077, 0, (2 << 16) | (1 << 8) | 0, 20), (0x81004008 + load, 0x81001100 + load, 1, 12)]
    hdr = struct.pack("<8s15I48s32s", b"GXVSAMP1", 1, index, 1000, 20000, 20000 * index, 0x81000000 + load, 0xe36590, 0x81000000 + load, len(ents), 0,
                      sum(e[3] for e in ents if not (e[2] >> 8) & 1), 20, 0, 5, 6, b"Galaxy BeachBowlGalaxy", b"port_sampler_boot")
    sl = slot(0x40010100, "game", 20000, 20000 * 9, 80, 0) + slot(0x40010101, "render", 20000, 20000 * 7, 60, 0) + b"".join(struct.pack("<6I24s", 0, 0, 0, 0, 0, 0, b"") for _ in range(10))
    body = b"".join(struct.pack("<4I", *e) for e in ents)
    open(path, "wb").write(hdr + sl + body)
b1, b2 = os.path.join(work, "sampler-1.bin"), os.path.join(work, "sampler-2.bin")
build(b1, 1, 0x10000); build(b2, 2, 0x10000)
r = subprocess.run([sys.executable, "-I", os.path.join(here, "sampler_report.py"), "unused.elf", b1, b2, "--nm-text", nm, "--top", "6", "--unmapped"], capture_output=True, text=True)
print(r.stdout); print(r.stderr)
assert r.returncode == 0
out = r.stdout
for need in ("load offset +0x10000", "thread game", "[Game/LiveActor] LiveActor::calcAnim()", "[Game/Player] MarioActor::movement()", "SomethingUnknown()", "wait semaphore",
             "Game/LiveActor", "stopped it 0.90%"):
    assert need in out, need
# self: calcAnim 120 of 2*? running samples; inclusive of calcAnim also gets its callees' samples (MR::getFrameLength, vgx::Submit and memcpy have lr in it)
assert "INCLUSIVE-BY-LR" in out
print("test_sampler_report: ok")

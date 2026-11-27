"""Export cropped gameplay-display thumbnails and the effective mode, not private fixtures."""
import binascii
from pathlib import Path
import struct
import zlib


def export(private, public):
    log = private / "game.log"
    if log.exists():
        rows = [l for l in log.read_text(errors="replace").splitlines() if "modekeys: Performance mode:" in l]
        if rows:
            (public / "modekeys.txt").write_text("\n".join(rows) + "\n")
    for tag in ("play", "return"):
        source = private / (tag + ".xwd")
        png = public / (tag + "-world.png")
        if not source.is_file() or png.exists():
            continue
        raw = source.read_bytes()
        if len(raw) < 100:
            continue
        h = struct.unpack(">25I", raw[:100])
        if (h[1], h[4], h[5], h[7], h[11]) != (7, 1280, 800, 0, 32):
            raise ValueError("Unexpected framebuffer")
        start = h[0] + 12*h[19]
        if len(raw) < start + h[5]*h[12]:
            continue
        # The isolated SDL game viewport fills the top-left 960x544 of Xvfb.
        # Exclude the emulator's own log window and downsample without PIL.
        rows = bytearray()
        for y in range(0, 544, 3):
            rows.append(0)
            for x in range(0, 960, 3):
                pos = start + y*h[12] + 4*x
                rows.extend(raw[pos:pos+3][::-1])
        def chunk(name, payload):
            return struct.pack(">I", len(payload)) + name + payload + struct.pack(">I", binascii.crc32(name+payload))
        png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320,182,8,2,0,0,0))
                        + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

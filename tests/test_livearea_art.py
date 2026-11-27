"""Tests for tools/vita/livearea_art.py with SYNTHETIC textures made here.  No game data is read or stored."""
import importlib.util
import io
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("livearea_art", ROOT / "tools/vita/livearea_art.py")
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


# ----- tiny GX encoders (the inverse of the decoder, written independently) -----
def tiles(w, h, bw, bh):
    for ty in range((h + bh - 1) // bh):
        for tx in range((w + bw - 1) // bw):
            yield tx * bw, ty * bh


def px(img, x, y, w, h):
    return img[min(y, h - 1) * w + min(x, w - 1)]


def enc_rgba8(img, w, h):
    out = bytearray()
    for x0, y0 in tiles(w, h, 4, 4):
        blk = [px(img, x0 + (i & 3), y0 + (i >> 2), w, h) for i in range(16)]
        out += b"".join(bytes((p[3], p[0])) for p in blk) + b"".join(bytes((p[1], p[2])) for p in blk)
    return bytes(out)


def enc16(img, w, h, f):
    out = bytearray()
    for x0, y0 in tiles(w, h, 4, 4):
        for i in range(16):
            out += struct.pack(">H", f(px(img, x0 + (i & 3), y0 + (i >> 2), w, h)))
    return bytes(out)


def enc8(img, w, h, f):
    out = bytearray()
    for x0, y0 in tiles(w, h, 8, 4):
        for i in range(32):
            out.append(f(px(img, x0 + (i & 7), y0 + (i >> 3), w, h)))
    return bytes(out)


def enc4(img, w, h, f):
    out = bytearray()
    for x0, y0 in tiles(w, h, 8, 8):
        for i in range(0, 64, 2):
            a = f(px(img, x0 + (i & 7), y0 + (i >> 3), w, h))
            b = f(px(img, x0 + ((i + 1) & 7), y0 + ((i + 1) >> 3), w, h))
            out.append(a << 4 | b)
    return bytes(out)


def to565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def tpl(fmt, w, h, data, big=True, palette=None, pfmt=2):
    """A one-image TPL.  big=False gives the port's cooked layout (headers little-endian, texels big-endian)."""
    e = ">" if big else "<"
    pal_hdr = 0x0C + 8 + 0x24 + 0x0C if palette else 0
    img_hdr = 0x14
    img_data = 0x60
    out = bytearray(0x60)
    struct.pack_into(e + "III", out, 0, 0x0020AF30, 1, 0xC)
    if not big:
        out[0:4] = b"\x30\xaf\x20\x00"
    struct.pack_into(e + "II", out, 0xC, img_hdr, 0x50 if palette else 0)
    struct.pack_into(e + "HHII", out, img_hdr, h, w, fmt, img_data)
    if palette:
        struct.pack_into(e + "HHII", out, 0x50, len(palette), 0, pfmt, 0x60 + len(data) + (-len(data) % 32))
    out += data
    if palette:
        out += bytes(-len(data) % 32)
        for p in palette:
            out += struct.pack(">H", p)
    return bytes(out)


def yaz0_literal(data):
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append(((0xFF00 >> len(chunk)) & 0xFF) if len(chunk) < 8 else 0xFF)
        out += chunk
    return bytes(out)


def rarc(files, big=True):
    """files: [(name, bytes)] placed in root dir 'root'.  big=False gives the cooked CRAR layout."""
    e = ">" if big else "<"
    names = b"\0"
    nameoff = {}

    def nm(s):
        nonlocal names
        if s not in nameoff:
            nameoff[s] = len(names)
            names += s.encode() + b"\0"
        return nameoff[s]

    entries = []
    blob = bytearray()
    for n, d in files:
        entries.append((nm(n), len(blob), len(d), 0x11))
        blob += d + bytes(-len(d) % 32)
    root_name = nm("root")
    dot, dotdot = nm("."), nm("..")
    entries += [(dot, 0, 16, 0x02), (dotdot, 0xFFFFFFFF, 16, 0x02)]
    nfiles = len(entries)
    info = 0x20
    node_off, file_off = 0x40, 0x40 + 16
    str_off = file_off + 0x14 * nfiles
    str_off += -str_off % 4
    hdr = bytearray(info + (str_off + len(names) - info))
    hdr[0:4] = b"RARC" if big else b"CRAR"
    data_off = (str_off + len(names) + 31) & ~31
    struct.pack_into(e + "I", hdr, 8, info)
    struct.pack_into(e + "I", hdr, 0xC, data_off - info)
    struct.pack_into(e + "IIIIII", hdr, info, 1, node_off - info, nfiles, file_off - info, len(names), str_off - info)
    struct.pack_into(e + "4sIHHI", hdr, node_off, b"ROOT", root_name, 0, nfiles, 0)
    for i, (no, off, size, flags) in enumerate(entries):
        f = file_off + 0x14 * i
        struct.pack_into(e + "HH", hdr, f, i, 0)
        if big:
            struct.pack_into(e + "HH", hdr, f + 4, flags << 8, no)
        else:
            struct.pack_into(e + "HH", hdr, f + 4, no, flags << 8)
        struct.pack_into(e + "II", hdr, f + 8, off, size)
    hdr[str_off:str_off + len(names)] = names
    hdr += bytes(data_off - len(hdr))
    return bytes(hdr + blob)


def logo_pixels(w, h):
    """A transparent-bordered coloured test logo, RGBA tuples with values representable in RGB5A3 opaque form."""
    img = []
    for y in range(h):
        for x in range(w):
            inside = 4 <= x < w - 4 and 3 <= y < h - 3
            img.append(((x * 8) & 0xF8, (y * 16) & 0xF8, 0xF8 if (x // 4) % 2 else 0x40, 255) if inside else (0, 0, 0, 0))
    return img


def flat(img):
    return b"".join(bytes(p) for p in img)


class DecoderTests(unittest.TestCase):
    W, H = 20, 11   # not a multiple of any block size

    def roundtrip(self, fmt, data, want, **kw):
        got = art.decode_gx(fmt, self.W, self.H, data, **kw)
        self.assertEqual(got, flat(want))

    def test_rgba8(self):
        img = [((x * 13) & 255, (y * 29) & 255, (x * y) & 255, 255 - x * 7) for y in range(self.H) for x in range(self.W)]
        self.roundtrip(6, enc_rgba8(img, self.W, self.H), img)

    def test_rgb565(self):
        e5, e6 = art._e5, art._e6
        img = [(e5(x % 32), e6((y * 5) % 64), e5((x + y) % 32), 255) for y in range(self.H) for x in range(self.W)]
        data = enc16(img, self.W, self.H, lambda p: to565(*p[:3]))
        self.roundtrip(4, data, img)

    def test_rgb5a3_opaque_and_translucent(self):
        e5, e4, e3 = art._e5, art._e4, art._e3
        img = []
        for y in range(self.H):
            for x in range(self.W):
                img.append((e5(x), e5(y), e5(31 - x), 255) if (x + y) % 2 else (e4(x % 16), e4(y % 16), e4(7), e3((x + y) % 8)))

        def f(p):
            if p[3] == 255 and (p[0] == art._e5(p[0] >> 3)):
                return 0x8000 | (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3)
            return (p[3] >> 5) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | p[2] >> 4
        self.roundtrip(5, enc16(img, self.W, self.H, f), img)

    def test_ia8_i8_ia4_i4(self):
        v = lambda x, y: (x * 12 + y * 5) & 255
        ia8 = [(v(x, y),) * 3 + (255 - v(y, x),) for y in range(self.H) for x in range(self.W)]
        self.roundtrip(3, enc16(ia8, self.W, self.H, lambda p: p[3] << 8 | p[0]), ia8)
        i8 = [(v(x, y),) * 4 for y in range(self.H) for x in range(self.W)]
        self.roundtrip(1, enc8(i8, self.W, self.H, lambda p: p[0]), i8)
        n = lambda x, y: (x + 3 * y) % 16
        ia4 = [(art._e4(n(x, y)),) * 3 + (art._e4(n(y, x)),) for y in range(self.H) for x in range(self.W)]
        self.roundtrip(2, enc8(ia4, self.W, self.H, lambda p: (p[3] // 17) << 4 | p[0] // 17), ia4)
        i4 = [(art._e4(n(x, y)),) * 4 for y in range(self.H) for x in range(self.W)]
        self.roundtrip(0, enc4(i4, self.W, self.H, lambda p: p[0] // 17), i4)

    def test_palette_formats(self):
        pal = [0x8000 | (i * 2) << 10 | (31 - i * 2) << 5 | i for i in range(16)]
        rgba = art.decode_palette(b"".join(struct.pack(">H", p) for p in pal), 2)
        idx = lambda x, y: (x * 3 + y) % 16
        want = [rgba[idx(x, y)] for y in range(self.H) for x in range(self.W)]
        self.roundtrip(8, enc4(want, self.W, self.H, lambda p: rgba.index(p)), want, palette=rgba)
        want8 = [rgba[idx(x, y)] for y in range(self.H) for x in range(self.W)]
        self.roundtrip(9, enc8(want8, self.W, self.H, lambda p: rgba.index(p)), want8, palette=rgba)

    def test_cmpr(self):
        # one 8x8 macro block: sub-blocks TL, TR, BL, BR with distinct colours and index patterns
        red, blue = to565(248, 0, 0), to565(0, 0, 248)
        data = b""
        for c0, c1, rows in ((red, blue, (0b00011011,) * 4), (red, blue, (0b11100100,) * 4),
                              (red, blue, (0xFF,) * 4), (blue, red, (0x00,) * 4)):
            data += struct.pack(">HH", c0, c1) + bytes(rows)
        out = art.decode_gx(0xE, 8, 8, data)
        pix = lambda x, y: tuple(out[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4])
        self.assertEqual([pix(c, 0) for c in range(4)],                    # top-left: c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1
                         [(255, 0, 0, 255), (0, 0, 255, 255), (170, 0, 85, 255), (85, 0, 170, 255)])
        self.assertEqual([pix(4 + c, 3) for c in range(4)],                # top-right: own block, indices 3,2,1,0
                         [(85, 0, 170, 255), (170, 0, 85, 255), (0, 0, 255, 255), (255, 0, 0, 255)])
        self.assertEqual(pix(0, 4), (85, 0, 170, 255))                      # bottom-left: index 3 everywhere
        self.assertEqual(pix(7, 7), (0, 0, 255, 255))                       # bottom-right: index 0 = c0 = blue

    def test_cmpr_transparent_mode(self):
        c0, c1 = to565(0, 0, 0), to565(248, 252, 248)           # c0 <= c1: 3 colours + transparent
        data = (struct.pack(">HH", c0, c1) + bytes((0b11100100,) * 4)) * 4
        out = art.decode_gx(0xE, 8, 8, data)
        # pixels use indices 3, 2, 1, 0 left to right
        self.assertEqual(tuple(out[0:4]), (0, 0, 0, 0))         # index 3 is transparent
        self.assertEqual(tuple(out[4:8]), (127, 127, 127, 255)) # index 2 is the average
        self.assertEqual(tuple(out[8:12]), (255, 255, 255, 255))
        self.assertEqual(tuple(out[12:16]), (0, 0, 0, 255))

    def test_errors(self):
        with self.assertRaises(art.ArtError):
            art.decode_gx(7, 8, 8, bytes(64))
        with self.assertRaises(art.ArtError):
            art.decode_gx(6, 8, 8, bytes(10))
        with self.assertRaises(art.ArtError):
            art.decode_gx(9, 8, 8, bytes(64))


class ArchiveTests(unittest.TestCase):
    def test_tpl_both_endians(self):
        img = logo_pixels(12, 8)
        data = enc_rgba8(img, 12, 8)
        for big in (True, False):
            w, h, rgba, name = art.parse_tpl(tpl(6, 12, 8, data, big=big))
            self.assertEqual((w, h, name), (12, 8, "RGBA8"))
            self.assertEqual(rgba, flat(img))

    def test_rarc_yaz0_both_layouts(self):
        for big in (True, False):
            arc = rarc([("a.txt", b"hello"), ("mytitlelogo.tpl", b"x" * 70)], big=big)
            for blob in (arc, yaz0_literal(arc)):
                files = dict(art.rarc_files(blob))
                self.assertEqual(files["root/a.txt"], b"hello")
                self.assertEqual(files["root/mytitlelogo.tpl"], b"x" * 70)

    def test_yaz0_backreference(self):
        # "ab" then copy 6 bytes from distance 2 -> "abababab"
        blob = b"Yaz0" + struct.pack(">I", 8) + bytes(8) + bytes((0b11000000,)) + b"ab" + bytes((0x40 | 0x00, 0x01))
        self.assertEqual(art.yaz0_decompress(blob), b"abababab")


@unittest.skipIf(Image is None, "Pillow not installed")
class ComposeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="art30test_")
        cls.addClassCleanup(lambda: __import__("shutil").rmtree(cls.root, ignore_errors=True))
        img = logo_pixels(60, 30)
        tex = tpl(6, 60, 30, enc_rgba8(img, 60, 30), big=False)
        arc = rarc([("mytitlelogo.tpl", tex), ("other.tpl", tex)], big=False)
        os.makedirs(os.path.join(cls.root, "files", "LayoutData"))
        with open(os.path.join(cls.root, "files", "LayoutData", "TitleLogo.arc"), "wb") as f:
            f.write(arc)

    def test_load_logo_crops_to_visible_part(self):
        info, logo = art.load_logo(self.root)
        self.assertEqual(info["size"], (60, 30))
        self.assertEqual(logo.size, (52, 24))

    def test_sizes_and_palettes(self):
        _, logo = art.load_logo(self.root)
        out = art.build_art(logo, pic0=True)
        self.assertEqual(set(out), set(art.SIZES))
        for path, blob in out.items():
            im = Image.open(io.BytesIO(blob))
            self.assertEqual(im.size, art.SIZES[path])
            self.assertEqual(im.mode, "P", path)
            self.assertEqual(blob[24], 8)
            self.assertEqual(blob[25], 3)
            art.check_png(blob, art.SIZES[path])
            self.assertGreater(len(set(im.convert("RGB").getdata())), 20)   # not a flat picture

    def test_no_stars_on_the_logo(self):
        from PIL import ImageChops, ImageFilter
        _, logo = art.load_logo(self.root)
        for path, fill, seed in ((art.ICON, 0.95, 11), (art.BG, 0.56, 22), (art.STARTUP, 0.88, 33), (art.PIC0, 0.56, 44)):
            size = art.SIZES[path]
            small, pos, avoid = art.logo_layout(logo, size, fill)
            stars = art.starfield(size, seed, avoid)
            plain = art.background(size)
            # the logo's own pixels plus a margin of 3 px: the starfield there is the plain glow
            own = Image.new("L", size, 0)
            own.paste(small.getchannel("A").point(lambda a: 255 if a > 8 else 0), pos)
            zone = own.filter(ImageFilter.MaxFilter(7))
            diff = ImageChops.difference(stars, plain).convert("L").point(lambda v: 255 if v else 0)
            self.assertIsNone(ImageChops.multiply(diff, zone.point(lambda v: 255 if v else 0)).getbbox(), path)
            self.assertIsNotNone(diff.getbbox(), path)         # but there are stars elsewhere
            self.assertEqual(art.starfield(size, seed, avoid).tobytes(), stars.tobytes())

    def test_icon_logo_fills_about_90_percent(self):
        _, logo = art.load_logo(self.root)
        small, pos, _ = art.logo_layout(logo, art.SIZES[art.ICON], 0.95)
        self.assertGreaterEqual(small.width, 0.9 * 128)
        self.assertGreaterEqual(pos[0], 0)
        self.assertLessEqual(pos[0] + small.width, 128)

    def test_deterministic(self):
        _, logo = art.load_logo(self.root)
        self.assertEqual(art.build_art(logo), art.build_art(logo))

    def test_check_png_rejects_wrong_things(self):
        buf = io.BytesIO()
        Image.new("RGB", (128, 128)).save(buf, "PNG")
        with self.assertRaises(art.ArtError):
            art.check_png(buf.getvalue(), (128, 128))
        buf = io.BytesIO()
        Image.new("P", (64, 64)).save(buf, "PNG")
        with self.assertRaises(art.ArtError):
            art.check_png(buf.getvalue(), (128, 128))

    def test_missing_files_message(self):
        empty = tempfile.mkdtemp(prefix="art30empty_")
        self.addCleanup(lambda: os.rmdir(empty))
        with self.assertRaises(art.ArtError) as cm:
            art.load_logo(empty)
        self.assertTrue(str(cm.exception).startswith("this needs your own Super Mario Galaxy files: "))
        with self.assertRaises(art.ArtError):
            art.load_logo(os.path.join(empty, "nope"))

    def make_vpk(self, path):
        entries = [("eboot.bin", b"\x7fELF" + bytes(300), zipfile.ZIP_DEFLATED),
                   ("sce_sys/param.sfo", b"PSF\0" + bytes(100), zipfile.ZIP_STORED),
                   ("sce_sys/icon0.png", b"old-icon", zipfile.ZIP_DEFLATED),
                   ("sce_sys/livearea/contents/template.xml", b"<?xml version='1.0'?><livearea/>", zipfile.ZIP_DEFLATED),
                   ("sce_sys/livearea/contents/bg.png", b"old-bg", zipfile.ZIP_DEFLATED),
                   ("sce_sys/livearea/contents/startup.png", b"old-startup", zipfile.ZIP_STORED),
                   ("module/x.suprx", os.urandom(500), zipfile.ZIP_DEFLATED)]
        with zipfile.ZipFile(path, "w") as z:
            for n, d, c in entries:
                zi = zipfile.ZipInfo(n, (2025, 3, 4, 5, 6, 8))
                zi.compress_type = c
                zi.external_attr = 0o644 << 16
                z.writestr(zi, d)

    def test_vpk_rewrite(self):
        _, logo = art.load_logo(self.root)
        new = art.build_art(logo, pic0=True)
        d = tempfile.mkdtemp(prefix="art30vpk_")
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        src, dst = os.path.join(d, "in.vpk"), os.path.join(d, "out.vpk")
        self.make_vpk(src)
        before = open(src, "rb").read()
        art.patch_vpk(src, dst, new)
        self.assertEqual(open(src, "rb").read(), before)          # input untouched
        with zipfile.ZipFile(src) as a, zipfile.ZipFile(dst) as b:
            self.assertIsNone(b.testzip())
            an, bn = a.namelist(), b.namelist()
            self.assertEqual(bn[:len(an)], an)                    # same order, extras appended
            self.assertEqual(bn[len(an):], ["sce_sys/pic0.png"])
            for n in an:
                if n in new:
                    self.assertEqual(b.read(n), new[n])
                else:
                    self.assertEqual(b.read(n), a.read(n), n)
                    self.assertEqual(b.getinfo(n).compress_type, a.getinfo(n).compress_type)
                    self.assertEqual(b.getinfo(n).date_time, a.getinfo(n).date_time)
            self.assertEqual(b.read("sce_sys/pic0.png"), new["sce_sys/pic0.png"])
        with self.assertRaises(art.ArtError):
            art.patch_vpk(src, src, new)

    def test_vpk_rejects_wrong_zip(self):
        d = tempfile.mkdtemp(prefix="art30vpk_")
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        bad = os.path.join(d, "bad.vpk")
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("readme.txt", "x")
        with self.assertRaises(art.ArtError):
            art.patch_vpk(bad, os.path.join(d, "o.vpk"), {})
        open(os.path.join(d, "notzip.vpk"), "wb").write(b"nope")
        with self.assertRaises(art.ArtError):
            art.patch_vpk(os.path.join(d, "notzip.vpk"), os.path.join(d, "o.vpk"), {})

    def test_cli_check_writes_nothing(self):
        before = set(os.listdir(self.root))
        self.assertEqual(art.main([self.root, "--check"]), 0)
        self.assertEqual(set(os.listdir(self.root)), before)
        self.assertEqual(art.main([os.path.join(self.root, "missing"), "--check"]), 1)


if __name__ == "__main__":
    unittest.main()

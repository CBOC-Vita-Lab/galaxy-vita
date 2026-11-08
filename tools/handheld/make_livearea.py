#!/usr/bin/env python3
"""The public package's bubble and LiveArea pictures: original drawings, no game artwork. A starry night sky over a glowing
ocean with a gold star (icon0.png 128x128, pic0.png 960x544, livearea/contents/bg.png 840x500, startup.png 280x158) and the
"Settings" tile (settings.png 280x124, the size the Vita's a1 LiveArea frame shows), all as 8-bit palette PNGs.
Reproducible: fixed seeds, the DejaVu Sans Bold font. The game replaces the bubble and backgrounds on the console with
pictures made from the player's own title screen (tests/test_title_art.py).
    python3 tools/handheld/make_livearea.py [output folder]"""
import math, os, random, sys, zlib
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'vita', 'vgx-game', 'sce_sys')
FONT = BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'

def sky(w, h, seed):
    rnd = random.Random(seed)
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = y / (h - 1)
            g = math.hypot((x - w * 0.62) / w, (y - h * 0.38) / h)
            glow = max(0.0, 1.0 - g * 2.2) ** 2
            px[x, y] = (int(8 + 22 * t + 70 * glow), int(10 + 14 * t + 40 * glow), int(38 + 52 * (1 - t) + 110 * glow))
    d = ImageDraw.Draw(im)
    for _ in range(w * h // 900):
        x, y = rnd.randrange(w), rnd.randrange(h)
        b = rnd.randrange(120, 256)
        r = 1 if rnd.random() < 0.85 else 2
        d.ellipse((x, y, x + r, y + r), fill=(b, b, min(255, b + 20)))
    return im

def noise(w, h, seed, octaves=5):
    rng = np.random.default_rng(seed)
    acc = np.zeros((h, w), np.float32); amp = 1.0; tot = 0
    for o in range(octaves):
        s = 2 ** (o + 4) * max(1, w // 840)
        g = rng.random((h // s + 2, w // s + 2)).astype(np.float32)
        im = Image.fromarray((g * 255).astype(np.uint8)).resize((w + s * 2, h + s * 2), Image.BICUBIC)
        acc += amp * (np.asarray(im, np.float32)[:h, :w] / 255.0); tot += amp; amp *= 0.55
    out = Image.fromarray((np.clip(acc / tot, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(max(2, w // 200)))
    return np.asarray(out, np.float32) / 255.0


def space(w, h, seed, cx=0.5, cy=0.5):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    t = y / h
    top = np.array([6, 8, 40], np.float32); bot = np.array([46, 14, 86], np.float32)
    img = top[None, None] * (1 - t[..., None]) + bot[None, None] * t[..., None]
    # nebula clouds: magenta and teal
    n1 = noise(w, h, seed); n2 = noise(w, h, seed + 7)
    d = np.sqrt(((x / w - cx) * 1.6) ** 2 + (y / h - cy) ** 2)
    swirl = np.clip(1.2 - d * 1.6, 0, 1)
    img += (np.clip(n1 - 0.45, 0, 1) * 2.2 * swirl)[..., None] * np.array([150, 40, 170], np.float32)
    img += (np.clip(n2 - 0.5, 0, 1) * 2.0)[..., None] * np.array([20, 110, 160], np.float32)
    # bright core glow behind the emblem
    core = np.exp(-(d / 0.28) ** 2)
    img += core[..., None] * np.array([90, 70, 150], np.float32)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    # star field
    rng = random.Random(seed)
    dr = ImageDraw.Draw(im)
    for _ in range(int(w * h / 900)):
        px, py = rng.random() * w, rng.random() * h
        b = rng.random() ** 3
        r = 0.6 + b * 1.6
        c = (int(200 + 55 * b), int(200 + 55 * b), 255, int(90 + 165 * b))
        dr.ellipse([px - r, py - r, px + r, py + r], fill=c)
    for _ in range(int(w * h / 22000) + 3):
        sparkle(im, rng.random() * w, rng.random() * h, 3 + rng.random() * 7 * (w / 840), (255, 255, 255))
    return im


def sparkle(im, x, y, r, col, alpha=230):
    lay = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(lay)
    d.polygon([(x, y - r), (x + r * 0.16, y - r * 0.16), (x + r, y), (x + r * 0.16, y + r * 0.16), (x, y + r),
               (x - r * 0.16, y + r * 0.16), (x - r, y), (x - r * 0.16, y - r * 0.16)], fill=col + (alpha,))
    glow = lay.filter(ImageFilter.GaussianBlur(r * 0.5))
    im.alpha_composite(glow); im.alpha_composite(lay)


def star_points(cx, cy, R, r, rot=-90):
    pts = []
    for i in range(10):
        a = math.radians(rot + i * 36)
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts


def emblem(im, cx, cy, R):
    W, H = im.size
    # orbit ring (behind)
    ring = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(ring)
    rw, rh = R * 2.3, R * 0.62
    d.ellipse([cx - rw, cy - rh + R * 0.1, cx + rw, cy + rh + R * 0.1], outline=(140, 220, 255, 200), width=max(2, int(R * 0.05)))
    ring_g = ring.filter(ImageFilter.GaussianBlur(R * 0.06))
    im.alpha_composite(ring_g); im.alpha_composite(ring)
    # halo
    halo = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(halo)
    d.polygon(star_points(cx, cy, R * 1.25, R * 0.62), fill=(255, 200, 60, 255))
    h2 = halo.filter(ImageFilter.GaussianBlur(R * 0.45)); a = np.asarray(h2).copy(); a[..., 3] = (a[..., 3] * 0.55).astype(np.uint8); im.alpha_composite(Image.fromarray(a))
    # star body with vertical gold gradient and rounded feel
    mask = Image.new("L", im.size, 0); ImageDraw.Draw(mask).polygon(star_points(cx, cy, R, R * 0.48), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(max(1, R * 0.012)))
    y = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    t = np.clip((y - (cy - R)) / (2 * R), 0, 1)
    top = np.array([255, 244, 150]); bot = np.array([255, 150, 20])
    grad = (top[None, None] * (1 - t[..., None]) + bot[None, None] * t[..., None]).astype(np.uint8)
    edge = Image.new("L", im.size, 0); ImageDraw.Draw(edge).polygon(star_points(cx, cy, R * 1.06, R * 0.53), fill=255)
    ol = Image.new("RGBA", im.size, (200, 90, 0, 0)); ol.putalpha(edge.filter(ImageFilter.GaussianBlur(max(1, R * 0.015)))); im.alpha_composite(ol)
    body = Image.fromarray(grad, "RGB").convert("RGBA"); body.putalpha(mask)
    im.alpha_composite(body)
    # inner highlight (smaller star, white, soft)
    hl = Image.new("L", im.size, 0); ImageDraw.Draw(hl).polygon(star_points(cx - R * 0.06, cy - R * 0.1, R * 0.55, R * 0.26), fill=150)
    hl = hl.filter(ImageFilter.GaussianBlur(R * 0.08))
    white = Image.new("RGBA", im.size, (255, 255, 240, 0)); white.putalpha(hl)
    im.alpha_composite(white)
    # front arc of the ring over the star's lower half
    front = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(front)
    d.arc([cx - rw, cy - rh + R * 0.1, cx + rw, cy + rh + R * 0.1], 15, 165, fill=(170, 235, 255, 230), width=max(2, int(R * 0.05)))
    im.alpha_composite(front.filter(ImageFilter.GaussianBlur(R * 0.04))); im.alpha_composite(front)
    # colourful sparkles orbiting
    rng = random.Random(int(R))
    cols = [(255, 120, 200), (120, 255, 170), (120, 190, 255), (255, 240, 120), (200, 140, 255)]
    for i in range(7):
        a = math.radians(200 + i * 22 + rng.random() * 8)
        sparkle(im, cx + rw * 0.95 * math.cos(a), cy + R * 0.1 + rh * 0.95 * math.sin(a), R * (0.09 + rng.random() * 0.07), cols[i % 5])


def title(im, text, cx, cy, size):
    f = ImageFont.truetype(FONT, size)
    lay = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(lay)
    bb = d.textbbox((0, 0), text, font=f); tw = bb[2] - bb[0]
    x, y = cx - tw / 2, cy - (bb[3] - bb[1]) / 2 - bb[1]
    glow = Image.new("RGBA", im.size, (0, 0, 0, 0)); ImageDraw.Draw(glow).text((x, y), text, font=f, fill=(120, 170, 255, 255))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(size * 0.18)))
    d.text((x, y + size * 0.06), text, font=f, fill=(20, 10, 60, 200))
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255))
    im.alpha_composite(lay)


# ---- v2: night sky over a calm glowing ocean (the look of the game's title screen, painted from scratch) ----
def sea_sky(w, h, seed, horizon=0.62):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    hy = horizon * h
    t = np.clip(y / hy, 0, 1)
    sky_top = np.array([4, 14, 60], np.float32); sky_low = np.array([40, 120, 210], np.float32)
    img = sky_top[None, None] * (1 - t[..., None] ** 1.6) + sky_low[None, None] * (t[..., None] ** 1.6)
    n = noise(w, h, seed)
    img += (np.clip(n - 0.5, 0, 1) * 1.4 * (1 - t))[..., None] * np.array([60, 40, 120], np.float32)
    # horizon glow
    g = np.exp(-((y - hy) / (h * 0.05)) ** 2)
    img += g[..., None] * np.array([140, 220, 255], np.float32)
    # ocean
    sea = y > hy
    s = np.clip((y - hy) / (h - hy), 0, 1)
    sea_col = np.array([70, 170, 230], np.float32)[None, None] * (1 - s[..., None]) + np.array([10, 40, 110], np.float32)[None, None] * s[..., None]
    waves = noise(w, h, seed + 3)
    rows = np.sin((y - hy) ** 0.8 * 1.3 + waves * 6) * 0.5 + 0.5
    sea_col += (rows * (1 - s) * 40)[..., None] * np.array([0.6, 0.9, 1.0], np.float32)
    # moon-path reflection column
    refl = np.exp(-((x - w * 0.5) / (w * 0.12)) ** 2) * (1 - s) * (0.4 + 0.6 * rows)
    sea_col += refl[..., None] * np.array([120, 170, 200], np.float32)
    img = np.where(sea[..., None], sea_col, img)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    rng = random.Random(seed); dr = ImageDraw.Draw(im)
    for _ in range(int(w * hy / 700)):
        px, py = rng.random() * w, rng.random() * hy * 0.92
        b = rng.random() ** 3; r = 0.5 + b * 1.5
        dr.ellipse([px - r, py - r, px + r, py + r], fill=(220, 235, 255, int(80 + 175 * b * (1 - py / hy * 0.6))))
    for _ in range(int(w * h / 30000) + 3):
        sparkle(im, rng.random() * w, rng.random() * hy * 0.8, 3 + rng.random() * 6 * (w / 840), (255, 255, 255))
    return im




def seed_of(name):
    return zlib.crc32(name.encode()) & 0xffff


def render2(name, w, h, star=None, text=None, ss=2, horizon=0.62):
    W, H = w * ss, h * ss
    im = sea_sky(W, H, seed=seed_of(name), horizon=horizon)
    if star:
        emblem(im, star[0] * W, star[1] * H, star[2] * H)
    if text:
        title(im, text[0], text[1] * W, text[2] * H, int(text[3] * H))
    return im.resize((w, h), Image.LANCZOS).convert("RGB")


def save(im, name):
    path = os.path.join(OUT, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert('RGB').quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def main():
    save(render2("icon0", 128, 128, star=(0.5, 0.45, 0.34), horizon=0.7), 'icon0.png')
    save(render2("pic0", 960, 544, text=("Super Mario Galaxy", 0.5, 0.3, 0.09)), 'pic0.png')
    save(render2("bg", 840, 500, text=("Super Mario Galaxy", 0.5, 0.3, 0.09)), 'livearea/contents/bg.png')
    save(render2("startup", 280, 158, star=(0.5, 0.36, 0.24), horizon=0.66), 'livearea/contents/startup.png')
    # the "Settings" tile of the LiveArea (280x124: the a1 frame's size; a 280x64 tile is not shown)
    im = sky(280, 124, 5)
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(BOLD, 30)
    w = d.textlength('Settings', font=f)
    d.text(((280 - w) / 2, 43), 'Settings', font=f, fill=(255, 255, 255), stroke_width=2, stroke_fill=(8, 30, 60))
    d.rounded_rectangle((1, 31, 278, 92), radius=10, outline=(255, 214, 64), width=2)
    save(im, 'livearea/contents/settings.png')


main()

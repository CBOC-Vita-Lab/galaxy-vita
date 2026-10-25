# HD textures (Mario, the main characters, key items)

Higher-resolution versions of about a hundred character and item textures, made from your own
game files with an upscaling network, stored GPU-ready (DXT1/DXT5 with their own mip chains) in
one pack file that the renderer reads on the console. The pack is made from your copy of the game:
it is never shipped, and it never goes in git. Keep it to yourself.

## Quick start

On a Windows, Mac or Linux computer with Python 3 (about 2 GB of free space, roughly 10-30 minutes):

1. Extract your disc with Dolphin ([how](../../MANUAL-PREPARE-GAME.md#1-extract-your-disc-with-dolphin)).
2. Make a flat converted copy for the builder (no movies needed):
   `python3 tools/cook/cook.py "/path/to/extracted" cooked`
3. Install the helpers and download the upscaling model (commands under **Rebuild** below).
4. Run `python3 -E -P tools/hdtex/build_pack.py --data cooked/files --out hdtex-build`.
5. Copy `hdtex-build/hdtex.pack` to `ux0:data/smg/hdtex.pack` on the Vita.

The game finds it at the next start. Delete the file, or add `GXV_HDTEX=0` to
`ux0:data/smg/env.txt`, to go back to the original textures.

## Rebuild

Needs Python 3 with numpy, Pillow, xxhash, quicktex and CPU PyTorch, and the Real-ESRGAN
weights `realesr-general-x4v3.pth` (xinntao/Real-ESRGAN release v0.2.5.0, BSD-3-Clause):

    pip install --user numpy pillow xxhash quicktex
    pip install --user torch --index-url https://download.pytorch.org/whl/cpu
    mkdir -p hdtex-build/models
    curl -L -o hdtex-build/models/realesr-general-x4v3.pth \
      https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth
    python3 -E -P tools/hdtex/build_pack.py            # about 10 minutes on 2 CPUs
    python3 -E -P tools/hdtex/build_pack.py --list     # only list what would go in

Options: `--data` (the `files/` folder of a flat converted copy; default `cooked/files`, or
`HDTEX_DATA`), `--out` (default `hdtex-build`), `--model` (default
`<out>/models/realesr-general-x4v3.pth`), `--scale 2|4`.

Output in `hdtex-build/`: `hdtex.pack`, `manifest.tsv` (every texture: names,
original and HD format/size, bytes, PSNR of the compression), `sheets/*.png` (before | after
for every texture), `cache/` (the network's output; a rerun only re-encodes).
Install: copy `hdtex.pack` to `ux0:data/smg/hdtex.pack`.

What goes in: `selection.txt` (archives) and the rules at the top of `build_pack.py`: colour
formats only (CMPR, RGB565, RGB5A3, RGBA8, C4/C8/C14X2); intensity formats (environment,
highlight, noise, indirect and mask maps), textures under 16x16, and names matching
`EXCLUDE_NAMES` are left alone; Low/Middle LOD models are not listed.

Per texture: decode level 0 (`gxdecode.py`), give fully transparent texels their neighbours'
colour, Real-ESRGAN 4x (realesr-general-x4v3: of the three models tried on these textures it
removed the CMPR block noise and kept edges clean without the outlines anime_6B adds or the
squaring x4plus does to small gradients, at 1/16 of x4plus's time), Lanczos down to 2x, mip
levels by area averaging down to 1x1, then BC1 (opaque) or BC3 (any alpha) with quicktex at
its best level. Three rules keep the result faithful to the original at the original's scale:
- Border padding for the network (`pad_mode`): wrap-around only when the texture really tiles
  across that seam (seam step no larger than twice the mean step between neighbouring texels);
  otherwise mirrored (clamped textures: edge). Many textures are flagged repeat but are atlases
  or gradients whose opposite borders differ; wrap-around padding showed the network a hard
  step there, which it sharpened like any edge, darker on one side and brighter on the other
  (the Power Star's dark right and bottom band, the Life power-up's top/bottom).
- Fidelity pass (`fidelity`): soft-thresholded back-projection. Where the HD level 0,
  area-averaged to the original's size, differs from the original by more than 4/255, the
  excess is spread back (bilinearly, 3 rounds). Corrects thin border lines the network washed
  out, uniform brightening of near-white areas and alpha rims, and keeps what the network adds
  below one texel and the CMPR block noise it removes.
- On/off alpha (all CMPR with transparency) stays on/off: the HD alpha is thresholded at 0.5.

Checks:
- `python3 -E -P tools/hdtex/check_borders.py <pack> [out.tsv]` (reads the converted copy from `HDTEX_DATA`, default `cooked/files`): for all entries, the HD level 0
  area-averaged to the original's size against the original, per border strip (top, bottom,
  left, right; 1 texel) and the interior: mean absolute difference and signed bias. Flags a
  texture whose border strip has MAD > 2 x interior MAD + 6 or |bias| > 6 (exit status 1).
- `python3 -E -P tools/hdtex/compare.py <pack> <out.png> Mario:MarioFace ...` validates every
  index entry of a pack against the selection (keys, offsets, level sizes) and draws close-up
  before/after pictures (both bilinear-magnified to one size).

## Pack format (version 1, little-endian)

    header, 32 bytes:  char magic[4] "HDTX"; u32 version = 1; u32 count; u32 indexOffset;
                       u32 namesOffset; u32 dataOffset; u32 fileSize; u32 reserved
    index, count x 32 bytes, sorted by key:
                       u64 key; u32 dataOffset (from the file start); u32 dataSize;
                       u16 width, height (replacement level 0); u16 origWidth, origHeight;
                       u8 format (1 = DXT1, 3 = DXT5); u8 levels (down to 1x1);
                       u8 origGxFormat; u8 flags (bit 0: alpha); u32 nameOffset (into names)
    names:             NUL-terminated "Archive:texture ..." strings (for the log)
    data:              per texture, 64-byte aligned: the levels, level 0 first, each a whole
                       number of 4x4 blocks (8 bytes DXT1, 16 bytes DXT5), as
                       glCompressedTexImage2D takes them

    key = XXH3_64(level-0 texel bytes as the GPU reads them (big-endian, tiled),
                  seed = gxFormat | width << 8 | height << 20)
    CI formats: key = XXH3_64(palette bytes, seed = key)

The key depends only on the texture's own bytes, so a texture is found wherever the game loads
it from, and a texture the game rewrites (or an EFB copy, which the cache resolves before) is
never replaced.

## Run time (platform/vgx/src/vgx_hdtex.cpp)

Loaded at start-up (`vgx::hdtex::Install`, from `vgx_galaxy.cpp`): the index (and, by default,
the whole pack: 4.5 MiB of main memory, so no texture's first use during play reads the card). The texture
cache asks on each upload of a guest texture (first bind, or changed bytes); never per frame or
per bind. Only textures whose format and size occur in the pack are hashed. A hit's data comes
from the preloaded pack (`GXV_HDTEX_PRELOAD`, default up to 8 MiB) or is read from the card,
and is handed to the back end as DXT1/DXT5 with its mip chain
(on the Vita down to the last level of at least 4x4). The guest-visible size stays the original's
(TEV texel sizes, texture matrices); replaced textures are sampled with
GL_LINEAR_MIPMAP_NEAREST, so wherever the texture is drawn at or below its original resolution
the GPU reads the second level, which has the original's size.

Switches (env.txt): `GXV_HDTEX` (0 off, 1 on when the pack exists: default, 2 dry run: count
hits, upload originals), `GXV_HDTEX_PATH`, `GXV_HDTEX_BUDGET` (MiB alive at once, default 16),
`GXV_HDTEX_PRELOAD` (MiB, default 8: a pack up to that size is read whole at start-up, so
no first use during play waits on the card; 0: read each texture on first use), `GXV_HDTEX_TRILINEAR`, `GXV_HDTEX_LOG`
(replacements logged by name, default 48). Log: `hdtex:` lines at start and per replacement,
`perf: hdtex:` with the counters every perf period, and a line in the texture-cache report.

Test: `tools/test_vgx_hdtex.sh` (synthetic pack through the real texture cache: replace,
mip clamp, budget, dry run, EFB copy, rewritten texture).

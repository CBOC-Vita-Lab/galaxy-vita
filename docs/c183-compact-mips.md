# Compact generated mipmaps

The Vita normally stores a generated I8, IA4 or IA8 texture as four bytes per
texel, repeating the luminance in RGB. C183 submits two bytes (luminance,
alpha) for unpatched square, power-of-two 8..1024 textures whose chain was
already scheduled to be generated, and only after the render backend approves.
No shader, color value, mip filter, max LOD or draw distance changes. Authored
I4, HD replacements, patch-hook textures and the separate mipmapped 16-bit
packing experiment do not take this route. Non-mipmapped textures are unchanged.

The first eligible upload under a covered screen runs the driver's two- and
four-channel mip generation against the same random image. It compares every
texel, including the padded rows of the 4x4 and 2x2 levels. A known nonzero
quadrant and distinct destination sentinels reject a driver that returns success
but writes nothing or clears both results. If the screen opens between scheduling
and upload, that upload goes as RGBA8 and the probe waits. A rejected probe leaves
the old RGBA8 route active. `GXV_LA88_MIPS=0` restores baseline handling.

fix31 (owner, 17 Nov): the default is `GXV_LA88_MIPS=0` (RGBA8, as c181) until the console A/B passes;
`GXV_LA88_MIPS=1` in `env.txt` is the A/B arm (`docs/RELEASE-3.0-CHECKLIST.md`, section 0b).

In an older physical run (`757-hL1/31/log.txt:186`) the previous, less strict
64-to-8 probe passed. The C183 check is stronger; that log does not establish
that C183 took this route, or increased FPS on hardware.

## Evidence

- `bash tools/test_vgx_la88_mips.sh`: real null-backend frontend resolves with
  the switch off, checked, and diagnostic-unchecked; live first use, quiet first
  use, positive/rejected verdicts. Decode compares the sampled (L,L,L,A) byte
  values with the production RGBA decoder across 100 format/size combinations
  and every I8, IA4 and IA8 source code. The actual Vita probe body runs with a
  software stand-in: correct padded transfers accepted, refusal, no write,
  all-zero write, corruption confined to 2x2, and failed allocation rejected.
- `python3 tools/c183_la88_assets.py /path/to/cooked/ObjectData`: 86 matching
  TEX1 records, 11 distinct eligible level-zero payloads (9 I8, 2 IA8) in the
  local cooked collection. If each is submitted once unmodified, their level-zero
  uploads use **679,936 fewer bytes**; their complete swizzled chains occupy
  **906,552 fewer bytes** than their RGBA8 twins. These are asset counts and
  byte arithmetic, not simultaneous residency or measured GPU memory traffic.
- Runtime `compact mips:` counts accepted submissions and level-zero bytes saved,
  cumulative at the first and every 64th upload. Compare the same stage, camera,
  clocks and cache first with 0 then with 1; neither record counts nor the 50%
  per-eligible-texture byte reduction establishes a frame-time or FPS gain.

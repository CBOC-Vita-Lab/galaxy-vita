# C185 Depth-Edge Diagnostic

## Status

`GXV_HRW41_DEPTH_EDGE=0` remains the default. This is an opt-in QA treatment for
one reproduced reprojection defect, not an established fix for the reported
lily/flower/Luma vibration. Quality/30 normally has `GXV_HRW=0`; this path cannot
explain that mode's report. No hardware test or Vita performance claim was made.

The existing C184 emulator `hrw31` PPM dumps are entirely zero RGB, so they are
not valid visual evidence. Older nonzero physical flipbooks predate the current
depth-aware warp and cannot establish this defect in C184 gameplay either.

## Mechanism

The HRW41 quarter-size mask stores two-channel logarithmic depth. A linearly
filtered sample between a near cutout and far scenery invents an intermediate
surface. The scene warp can then fetch an unrelated source pixel while the
camera moves. C182 already avoided this classification error in Mario's box
key; the full-scene warp still decoded the filtered depth directly.

The diagnostic also reads the actual nearest mask texel at its center. Only when
that texel is farther than the filtered sample by at least `1/64` code (about
17.6% distance) does it use the texel's depth. It does not pull the near side
forward, change the mask, blur color, lower the picture rate, or modify the
Mario box, player-occlusion shader, or HUD. The threshold is a QA bound, not a
general proof of a discontinuity: extremely steep smooth surfaces remain a risk.

A symmetric nearest-texel treatment was rejected: it slightly increased error
for the wide CutBush fixture. Nearest sampling everywhere was also rejected
because it can staircase smooth surfaces.

## Pixel Evidence

`tools/test_c185_depth_edges.sh` extracts and rasterizes the real production
`MaskFragment`, `D41Fragment`, and existing box/occlusion shaders using Mesa GLES.
Its oracle is an independent redraw of known near/far layers, not a copy of the
warp algorithm. The 64x64 FlowerBlue and 128x128 CutBush RGBA inputs are decoded
from the user's cooked assets, retain their binary alpha silhouettes, and stay
outside the source/distributable under `outputs/galaxy-c185/private-plant-fixtures`.
The fixture is not a complete gameplay material, camera, simulation or Luma test.
Private raw RGBA SHA256: FlowerBlue
`0fa0a86c993de7f81a598a04ab70aeda1bfcd9d1c3a692c5d3549b539c5f60d0`;
CutBush `d8c12a71e4f1f7f028f9dd4f9a652d78798d8891ec53a2ce2c394886e8a9600f`.

| Fixture | Absolute RGB Error/Channel, Off -> On | Inter-Pan Residual Change, Off -> On |
| --- | --- | --- |
| FlowerBlue, 48px wide | 5.318 -> 4.888 | 5.409 -> 4.890 |
| FlowerBlue, glancing 18px wide | 4.498 -> 4.236 | 4.792 -> 4.421 |
| CutBush, 48px wide | 5.896 -> 5.464 | 6.004 -> 5.500 |
| CutBush, glancing 18px wide | 4.374 -> 4.118 | 4.631 -> 4.310 |

Each row checks 42,560 pixels across ten signed slow/fast pan offsets from
0.25px to 4px, with every tested pan improving. The independently composited
fresh-player/HUD proxy regions have zero changed pixels. These are explicit
opaque overlay proxies, not a claim that a real Mario framebuffer was tested.
Eighteen flat/tilted-surface diagonal pan cases are byte-identical with the
treatment enabled. Eight mask-grid phases of a two-surface edge improve from
21.577 to 14.671 levels/channel across 8,400 pixels. Eight disabled shader
variants retain their frozen C184 text hashes; all optional variants compile.

Full receipt: `outputs/galaxy-c185/depth-edge-pixels.log`. Framebuffer PPMs:
`outputs/galaxy-c185/private-depth-edge-pixels/`.

## Cost And Rollback

Disabled: unchanged shader text/cache keys, no extra texture sample, no extra
per-frame uniform call, no new pass, mask allocation or scene boundary.

Enabled: HRW41 depth mode 1 adds one mask sample per scene-warp fragment
(two mask reads plus one color read in total). Depth mode 2 adds two mask
samples (four mask reads plus one color read). One mask-size uniform is uploaded
per affected warp draw. No Vita GPU cost has been measured.

For a later explicitly authorized hardware comparison, set
`GXV_HRW41_DEPTH_EDGE=1` in `ux0:data/smg/env.txt` before launching the same build,
leaving all other settings and cache state matched. Compare pans in both
directions, flying, thin cutouts, tilted ground, disocclusions, Mario and HUD.
Rollback: remove that line or set it to `0`, then fully relaunch. Runtime
environment values are cached; changing the file in a running process does not
change this shader. Do not enable by default without real nonzero gameplay
color/depth captures and a hardware cost check.

## Reproduce Offline

The ordinary test works without private assets:

```sh
bash tools/test_c185_depth_edges.sh
```

For the existing local offline container and decoded private fixtures:

```sh
docker exec -e C185_FLOWER_RGBA=/out/private-plant-fixtures/FlowerBlue.rgba \
  -e C185_BUSH_RGBA=/out/private-plant-fixtures/CutBush.rgba \
  -e C185_REQUIRE_SILHOUETTE=1 -w /worktree galaxy-c185-build \
  bash tools/test_c185_depth_edges.sh /out/private-depth-edge-pixels
```

The optional RGBA files are exact unheaded 8-bit RGBA, 64x64 and 128x128,
respectively. No game assets are embedded in these tests or release packages.

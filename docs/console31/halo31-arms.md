# halo31: console arms (Performance + 60 Assist)

Written by agent halo31, 18 Nov 2026. Not yet made into relay jobs: `docs/CONSOLE-SESSION-3.1.md` and `tools/console31/make_jobs.py` have another
owner tonight. Each arm below is a whole `env.txt` (it replaces the card's env.txt for the run), on a build that holds branch `halo31`.

## What the arms are for

- **H arms:** `GXV_HRW_HALO`: the second picture without the player box (the "force field"). Needs no depth.
- **K arm:** the game's depth clear is now recognised (`depth-rect` in the refusal line). It shows how many first pictures keep their depth
  once only the HUD's depth-tested draw is left to refuse them.
- **W arms:** what makes the water shake (owner 21:44, 18 Nov). One variable each.

## Common lines (every arm; the same route as 8229 KD-B1 / 8223 S3)

```
PETARI_BOOT=1
PETARI_STAGE=HoneyBeeKingdomGalaxy:1,HeavenlyBeachGalaxy:1
PETARI_STAGE_HOLD=7200
PETARI_FLOW_LOG=1
PETARI_SCRIPT_ANCHOR=play
PETARI_SCRIPT_LOG=1
GXV_HRW_STAGE_AUTO=0
GXV_HRW_FRESH_AUTO=0
```

The last two keep the pairs and the fresh player on for the whole run (as the KD0/KD1/KD2 arms): the halo needs the fresh player, and a stage
that falls back to 30 shows nothing. Honeyhive 120 s, then Beach Bowl 90 s, the scripted route of 8229, three mid-motion screenshots a stage
(one while he runs, one in a jump, one during a camera pan). Card settings: Performance, 60 Assist on.

## Arms

| Arm | Lines added to the common ones | What it shows |
|---|---|---|
| H0 | `GXV_HRW_HALO=0` | Control: the c181 box on this build. The picture the owner calls the force field. |
| H1 | `GXV_HRW_HALO=1` | The halo: no box; the rim behind him filled from beside him. **The arm for the owner's eyes.** |
| H2 | `GXV_HRW_HALO=2` | As H1 with the filled pixels painted red. For the screenshots only: red must be a thin rim on the side he comes from. |
| H1b | `GXV_HRW_HALO=1` and `GXV_HRW_HALO_BANDS=1` | The cheapest form: one band (12 more stencil-only draws a second picture instead of up to 96). Run only if H1 costs frames. |
| H1p | `GXV_HRW_HALO=1` and `GXV_HRW36_PLANE=0` | The halo without the ground plane: tells the halo's effect apart from the plane's (and is the W1 water arm with the halo). |
| K | (none) | The default. Read the `hrw: keepdepth31` lines: see "What to read". |
| W0 | (none; the same run as K) | Water control: the default at Beach Bowl. |
| W1 | `GXV_HRW36_PLANE=0` | No ground plane. If the water is calm here, the plane moves it. |
| W2 | `GXV_INDCAP_HALFRATE=0` | The refraction capture every picture (the Beach Bowl row sets `indcap_half=2`: every second picture reads a capture one picture old). Check the `mode:` line that the env's 0 won over the stage row; if it did not, this arm says nothing. |
| W3 | `GXV_HRW=0` | 60 Assist off (a plain 60 target). If the water still shakes, the generated picture is not the cause. |

Order for the owner's eyes: H0, H1, H1 again by hand (below), then W0, W1, W2, W3. H2 and K are unattended.

### H1 by hand (2 minutes, the owner playing, Honeyhive start planet)

1. Run in a straight line with the camera following. Look at the ground right round his feet and legs.
2. Stand still, then jump several times next to a wall or tall grass. Look at the wall beside him as he rises.
3. Hold the camera stick to one side while running (a pan about him).
4. Walk to the pond's edge, then Beach Bowl: stand in the shallow water, walk, swim.

## What to read

**H arms** (every 300 pairs, about 10 s):
- `hrw: halo31 | GXV_HRW_HALO 1: pairs planned N: stamp and fill A, ... under 0.75 px apart B, refused ... over GXV_HRW_HALO_MAX_PX apart C, no source D,
  no box or no fresh player E | drawn with it F, not: no stencil / margins / debug tint G`
  - `drawn with it` should be near `stamp and fill`. `no stencil` must be 0 (else `GXV_SHADOW_STENCIL` is off or the EFB has no stencil: the box is drawn).
  - A large `no box or no fresh player`: the fresh player was left out (check `hrw: fresh auto`), the pair got the box.
- `hrw: halo31 cost | old player from the fresh one: mean / max px, lean ... | a picture drawn with it: bands, stencil-only draws, his own draws, ...
  stencil rectangle px, reset + fill passes ms`
  - `his own draws` should be 12 (console6). `stencil-only draws` = 12 x bands.
- The usual lines against H0: shown fps (59.9), `hrw: warp | ... warp pass ms`, `hrw: fresh deadline`, `be-split-5s` render-thread ms, hitches.
- `hrw: warp32 | player boxes N`: in H1 the count falls to the pairs that were refused (the box is not drawn on a halo pair).

**K arm:**
- `hrw: keepdepth31 refusal N event | ... rect 0,0 640x456 depth-rect, z 1.0000..1.0000, vs 440 fs 1170`: the word `depth-rect` says the front end
  marked the game's clear. If the clear is the deciding event at all, the mark is missing (the quad was not transformed on the CPU): report the line.
- `hrw: keepdepth31 refusal N | ... last depth-only clear at command C` with C a number, not -1.
- `hrw: keepdepth31 | first pictures planned 900: depth clear left out X; refused: ... depth read after it Y, depth written after it Z`.
  Before: X 0, Y 616-900, Z 0-283. Now Z should be 0 and X = the pictures in which the HUD draws nothing that tests depth. `depth-tested EFB draws
  after a left-out clear` must be 0.
- If X > 0: `hrw: keepdepth31 B | ... second pictures with the kept depth: fresh player N`: in those pairs Mario is hidden by what stands in front
  of him. Watch for him flickering behind grass (kept in some pairs, not in others). `GXV_HRW_KEEPDEPTH=0` is the way back.

**W arms:** the owner's eyes on the water, plus, per arm, the `hrw: warp36 | ground parallax at the player ... pairs on his ground plane N ... no shadow
polygon M` lines of the Beach Bowl part and `hrw: ground38 | ... (riding, swimming, standing, jumping, other)`.

## Pass rules

- **H1 passes** if the owner no longer sees an area round Mario that moves differently from its surroundings (steps 1-3 by hand), Mario is not
  doubled, shown fps is 59.9 as in H0, there is no new hitch class and no crash dump, and `no stencil` is 0.
- **H1 fails** (send the screenshots and both halo31 lines) if any of these shows:
  - the fresh Mario is covered by background or, in H2, painted red: the stencil compare is not doing on the console what it does in GL;
  - red in H2 fills a rectangle instead of a rim: the stamp reached every pixel (stencil reset or compare wrong);
  - Mario is doubled as with no box at all: the stamp or the fill is not drawn (`stencil-only draws` 0, or `drawn with it` 0);
  - fps or evenness falls against H0: try H1b.
- **K passes** if `depth clear left out` is above 0, `depth-tested EFB draws after a left-out clear` is 0 and nothing looks different in the
  first pictures. It is expected to stay at 0 kept while the HUD's depth-tested draw is in every picture (console6: 900 of 900); the line then says
  `depth read after it 900` and `depth written after it 0`, which is the proof that only the HUD draw is left.
- **W arms:** the arm in which the water is calm names the cause. If none is calm, including W3, the shaking is in the first pictures too and
  is not the generated picture's.

# Performance on the PS Vita (version 2.0, November 2026)

Every number on this page was measured on a real PS Vita. Each run starts the game from the memory card, goes straight into
one galaxy and plays it for about 2½ minutes: running, jumping, spins and camera turns. The game's own log records the frame
rate and the picture size every 5 seconds.

- **Typical** is the median of the last 40 seconds of 5-second stretches.
- **Low** is the slowest of those stretches.
- The picture size is how wide the game drew most of that time. The screen is 960×544; smaller pictures are scaled up to it.

The test Vita runs the CPU at 500 MHz (PSVshell) and has the CapUnlocker plug-in, which lets games use the Vita's fourth CPU
core. Without the fourth core the game still runs, but the busiest scenes are a few fps slower (the lit Comet Observatory
measured about 28 fps instead of 30).

## Picture modes

| Mode | What it aims for |
|---|---|
| **Quality** (default) | A steady 30 fps at up to the Vita's full 960×544. If the graphics chip falls behind, it first turns off edge smoothing, then lowers the sharpness one small step at a time. A slowdown caused by the game's own work never costs sharpness. When there's room again, everything comes back. |
| **Performance** | Up to 60 fps. Under load it gives up edge smoothing first, then picture size, down to 608×348. When 60 is out of reach, it shows an even 30 rather than an uneven 40, and then sizes the picture for 30, up to 720×404. |
| **Locked 30** | An even 30 fps, never below the full 960×544. Edges are smoothed with multisampling when there's room. |

## Quality (the default)

| Galaxy | Zoom 1.0× | Zoom 1.5× (default) | Zoom 2.0× |
|---|---|---|---|
| Good Egg | 30 fps, 848–960 wide | 30, 896–960 wide | 30, 960×544 |
| Beach Bowl | 30, 720–784 wide | 30, 720×404 | 30, 664–720 wide |
| Honeyhive | 30, 784–848 wide | 30, 848×480 | 30, 848×480 |
| Bowser's Star Reactor (Bowser 3) | 29, 784–848 wide | 30, 784×440 | 30, 784–896 wide |

The low stretches stay at 28–30 fps in every row, except Bowser's Star Reactor at 1.5×, which dips to 26 for a few seconds.

## Performance (up to 60)

| Galaxy | Zoom 1.0× | Zoom 1.5× | Zoom 2.0× |
|---|---|---|---|
| Good Egg | 54 fps, 664×376 | 60, 608–664 wide | 49, 664–720 wide |
| Beach Bowl | 30 (even), 608×348 | 42, 608–664 wide | 37, 608–720 wide |
| Honeyhive | 42, 664–720 wide | 54, 608×348 | 59, 608–720 wide |
| Bowser's Star Reactor | 45, 608–664 wide | 49, 608×348 | 48, 608×348 |

## Locked 30 (zoom 1.0×, always 960×544)

| Galaxy | Typical |
|---|---|
| Good Egg | 30 fps |
| Honeyhive | 30 |
| Bowser's Star Reactor | 30 |
| Beach Bowl | 25 (Locked 30 never lowers the size; Quality holds 30 here) |

## Compared with 1.2 (zoom 1.0×, the widest view and the hardest case)

| Galaxy | 1.2, default mode | 2.0, Quality | 2.0, Performance |
|---|---|---|---|
| Good Egg | 28 fps at 640×456 | **30 at 848–960 wide** | **54** at 664×376 |
| Beach Bowl | 30 at 544–640 wide | **30 at 720–784 wide** | 30 at 608×348 |
| Honeyhive | 12 at 544×388 | **30 at 784–848 wide** | **42** at 664–720 wide |
| Bowser's Star Reactor | 25 at 544×388 | **29 at 784–848 wide** | **45** at 608–664 wide |

| Galaxy | 1.2, Locked 30 | 2.0, Locked 30 |
|---|---|---|
| Good Egg | 30 at 640×456 | 30 at 960×544 |
| Beach Bowl | 15 | 25 at 960×544 |
| Honeyhive | 13 | 30 at 960×544 |
| Bowser's Star Reactor | 23 | 30 at 960×544 |

Full 960×544 is 1.8 times the pixels of 640×456 and 2.5 times those of 544×388.

The 1.2 figures come from a scripted tour of the same galaxies on the same Vita. The Vita's graphics cache held the newer build's
shaders at the time, so 1.2 prepared about 2,000 shaders during those runs, which makes its heaviest scenes (Honeyhive above
all) look worse than they played. 1.2's own published figures, measured with its package, were 46 fps in Good Egg, 38 in the
Observatory and 31–32 in Beach Bowl, Sea Slide and the first Bowser fight, at 640×456 and below.

In tours of the 2.0 test builds, the lit Comet Observatory held 30 fps at 960×544 in Quality and about 50 in Performance, and Sea
Slide held 30 at 720–848 wide in Quality.

## What changed since 1.2

- **The fourth CPU core** (with CapUnlocker): sound, part of the renderer's work, and the camera and animation work that used to
  wait for the main game thread now run on it. The game checks the core at start-up and retries if the system is busy.
- **Full-size pictures that hold.** Quality judges by what you see: it only lowers sharpness after pictures have actually come
  late because of the graphics chip for about 4 seconds, never for a single spike and never for a slowdown the CPU caused.
- **Edge smoothing as the first thing to go**, before any loss of sharpness, and back when there's room. In Performance mode its
  saving (about 3 ms of the graphics chip a picture) is no longer mistaken for noise.
- **Performance mode sizes the picture for the even 30** when 60 is out of reach, instead of shrinking it for a 60 it can't
  reach, and leaves the 30 again as soon as a smaller picture could do better.
- **Cheaper graphics work at full size:**
  - the edge-smoothing pass skips the HUD, which is redrawn anyway;
  - a full-screen pass per scene that vitaGL didn't need is skipped;
  - the blur passes of the bloom glow run at half size;
  - shaders leave out clamps and light terms proven to change nothing;
  - water discards pixels that change nothing early, and the sea's ripple scaling runs per vertex instead of per pixel.
- **Less work on the game's own thread:** collision, gravity, area and shadow queries reuse their answers while nothing changed
  (each checked on the console against the original, with no differences), flower patches animate once per shown picture, and
  several port-side costs are gone.
- **A fixed 1-pixel line** on odd-sized screens (vitaGL rounded half the viewport down).

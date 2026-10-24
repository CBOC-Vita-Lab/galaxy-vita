# Technical Notes

How the port works, the switches it reads at start-up, and the engineering notes for each version.
For building, see [Building](BUILDING.md); for what a release was built from, [Source Record](SOURCE.md).

## How It Works

- **The game** is the [Petari](https://github.com/SMGCommunity/Petari) decompilation of Super Mario
  Galaxy (`decomp/`), compiled as native 32-bit ARM code with Clang. Release builds link it with
  ThinLTO.
- **The platform layer** (`platform/`) stands in for the Wii's SDK: threads and alarms, memory,
  disc reads, audio (AX/DSP), Wii Remote and Nunchuk input, movies (THP) and the GX graphics API.
  `vita/runtime/` holds the Vita side: buttons, touch, motion, audio output, memory and the
  handheld controls.
- **The renderer** (`platform/vgx/`, "vgx") turns the game's GX command stream into vitaGL draws on
  the Vita's GPU. It runs on three cores: core 0 runs the game thread, core 2 the front end
  (the "player": parses the stream, resolves state, builds draws) and core 1 the back end (vitaGL
  submission), which also hosts the front end's finishing thread. Helper threads (audio, input,
  clocks) run on cores 1-2.
- **The game data** is the player's own disc, converted on the computer by `tools/cook/`
  (or by CBOC Vita Installer's `galaxy` step, which gives the same files): archives decompressed,
  big-endian structures made little-endian, formats the game reads as byte streams left as they
  are, and the four largest folders split into `_bkNN` bucket folders (FNV-1a of the lower-case
  name) so a card lookup reads 32 entries instead of 1,800. `sys/fst.bin` and the runtime agree on
  that layout.

### On The Memory Card

| Path | What it is |
| --- | --- |
| `ux0:app/SMGV00001/` | the app (`eboot.bin`, LiveArea, the compiled-shader seed in `shadercache/`) |
| `ux0:data/smg/game/` | the prepared game files (`sys/`, `files/`) |
| `ux0:data/smg/saves/` | saves |
| `ux0:data/smg/settings.ini` | pause-menu settings (camera zoom, rear touch aiming) |
| `ux0:data/smg/shadercache/` | vitaGL's compiled shaders (`<key>/v3/...`) and the shader warm-up list |
| `ux0:data/smg/hdtex.pack` | optional HD textures ([tools/hdtex](../tools/hdtex/README.md)) |
| `ux0:data/smg/env.txt` | optional `NAME=value` switches, read once at start-up |
| `ur0:data/libshacccg.suprx` | the shader compiler (the player's own) |

## Runtime Switches

Put one `NAME=value` per line in `ux0:data/smg/env.txt`. Defaults are the release's behaviour;
every switch keeps the older path reachable for comparison. The ones a player might want:

| Switch | Default | Effect |
| --- | --- | --- |
| `GXV_AUTOQ` | 1 | Automatic picture quality. `0` keeps the fixed 640x456 picture |
| `GXV_AUTOQ_MSAA` | 1 | The anti-aliased rungs of the automatic quality. `0` leaves them out |
| `GXV_AUTOQ_LEVEL` | -1 | A fixed rung, 0-8 (`-1`: automatic) |
| `GXV_AUTOQ_MIN` / `_MAX` | 0 / 8 | The lowest and highest rung the automatic quality may use |
| `GXV_HDTEX` | 1 | HD textures when `hdtex.pack` exists. `0` off |
| `GXV_SHARPEN` | auto | Sharpening of an upscaled picture: `auto`, `off` or an amount |
| `GXV_RENDER_SIZE` | | A fixed internal size such as `640x456` (also turns the automatic quality off) |
| `PETARI_SETTINGS_PANEL` | 1 | The settings panel in the pause menu. `0` hides it |
| `PETARI_SETTINGS_SE` | 1 | Its sounds. `0` silences them |
| `PETARI_REAR_ACCUM` | 1 | Rear touch aiming adds up slow slides. `0`: only faster slides move the pointer |
| `GXV_CHARAA` | 0 | Edge smoothing of the characters and key items only (FXAA in small boxes inside the final upscale, the HUD left alone). `1` on, `2` shows its coverage tinted red |

Developer switches (`GXV_*` in `platform/vgx/src/` and `platform/vgx/vita/vgx_host_galaxy_vita.cpp`,
`PETARI_*` in `platform/src/`) are documented where they are read. Many have a verify mode
(`=2`) that runs the fast path and the old path side by side and logs every difference:
`GXV_STATELIST`, `GXV_RSCACHE`, `GXV_REGSHADOW`, `GXV_TEXMEMO`, `GXV_MERGEFAST`, `GXV_SPLIT`.

The automatic quality's ladder (rungs 0-8): 544x388, 592x422, 640x456 (the Wii's EFB size and the
start), 720x480, 800x504, 880x520, 960x544, then 960x544 with the EFB multisampled 2x and 4x.

## What's In Version 1.0

Everything below is in the source of this release; figures are labelled
with where they were measured. "ARM harness" figures are instruction counts from running the
Vita build's ARM code under QEMU with a fixed input script (Comet Observatory, frames 8200-8500),
not console timings.

### Front end (the "player" thread)

- **State lists.** Material display lists that only set state are parsed once into host-order
  operations and replayed while their memory is unchanged. Plain BP writes are kept as runs and
  replayed with one compare each; the table uses open addressing (no division). A list whose
  memory was rewritten with the same bytes (J3D rewrites its materials every picture) is
  recognised by a kept copy instead of a hash (`GXV_SLBYTES`). `GXV_STATELIST=2` verified
  0 mismatches in 458,752 replays (Comet Observatory) and in emulator runs.
- **Resolution cache** (`GXV_RSCACHE`): a hash of exactly the registers the shader keys read,
  kept up to date by the register writers, maps to the shader ids through a 1,024-entry memo;
  render state, texture bindings and uniform blocks are rebuilt only from what they depend on.
- **Register no-op shadows** (`GXV_REGSHADOW`) for TEV colours and CP vertex formats, and a
  **texture binding memo** (`GXV_TEXMEMO`).
- **Bigger front caches:** shader registry 64 -> 1,024 slots, texture cache 128 -> 1,024, program
  front 64 -> 1,024 with a multiplicative hash; stage-effect tables found by hash; layout
  pane-name compares settle on the first character; `getMaterialAnm` skips the region scan for a
  null pointer.
- **Front-end split** (`GXV_SPLIT`, on by default on the Vita): uniform blocks, draw records and
  the CPU transform of small primitives are finished on a second thread (on the back end's core,
  one priority step above it). At a frame's end the player takes over whatever is left rather
  than waiting. `GXV_SPLIT=2` compares every frame with the single-thread path; a jitter mode
  and a ThreadSanitizer host test cover the hand-over.
- **Results (ARM harness, Comet Observatory):** the BP runs and table changes took the front end
  from 5.54 to 5.20 million instructions per picture; the resolution cache, shadows, memo and
  byte compares then took it from 5.26 to 4.47 million (measured with the harness clock held for
  the front end, `PETARI_VCLOCK_FE=1`, so both runs play the same frames). Every verify mode
  reported 0 mismatches.

### Game thread and build

- **ThinLTO for the game code:** the 1,834 decompiled units as bitcode, linked into one object
  with `--lto-O2`. Game thread -12 % in the festival plaza (ARM harness, 2.540 -> 2.234 million
  instructions per turn).
- The renderer is built at `-O3` (-3 % front-end instructions).
- Helper threads (audio, input, clocks) moved off the game thread's core 0 to cores 1-2
  (`GXV_HELPER_CORES=0` restores the old placement).

### Back end and GPU

- **Fewer GPU scenes per picture.** Every EFB copy used to end a GXM scene (store, then reload).
  A per-frame depth plan (`GXV_DEPTHPLAN`), swapping the EFB colour buffer instead of copying it
  (`GXV_EFBSWAP`), late direct passes (`GXV_PASSDIRECT2`) and a hoisted corner copy
  (`GXV_PASSHOIST`, only on whole render pixels) cut the number of scenes. The desktop build's
  in-process verifier (`GXV_PASSVERIFY`) checks each change frame by frame.
- Reduced-size additive effects (`GXV_FXHALF`) and half-size blur passes (`GXV_PASSHALF`) are in
  the source but **off** by default.
- **Video memory:** render targets are owned and bounded explicitly; idle pooled framebuffers are
  deleted after 60 presents (`GXV_POOL_IDLE`); textures are detached from pooled framebuffers
  before deletion (`GXV_DETACH_DELETE`), so vitaGL really frees them; no new EFB buffer or effect
  pass below 6 MiB free.
- **Shader failures recover:** a shader that does not compile, or a program that does not link,
  is deleted and retried after 60, 120 and 240 frames instead of holding one of vitaGL's 1,024
  program slots for good (`GXV_GL_RETRY`).

### Automatic picture quality

- **Dynamic resolution** (`GXV_AUTOQ`, on by default on the Vita): the back end's render size
  follows the ladder above. A watcher thread times each scene on the GPU (vitaGL `GPU_TIMERS`), so
  the controller can tell a GPU limit from a CPU limit. A change is applied between frames: the
  EFB and every render-target texture are resampled to the new size and keep their pictures.
- **Anti-aliasing rungs:** at 960x544, with GPU time and video memory to spare, the EFB alone is
  multisampled 2x or 4x (vitaGL `FBO_MSAA`); the resolved picture is what copies read.
- **Sharpened present:** a smaller render is upscaled with a light unsharp mask (`GXV_SHARPEN`).
- **Calm rules** (`GXV_AUTOQ_CALM=1`, from the first console runs): the first console log showed
  24 changes in 7 minutes, each freezing the picture 100-270 ms (5.7 s of hitches in all), and an
  Observatory that oscillated between sizes. Now a step up must fit 70 % of the frame budget
  across a whole stretch of room and waits for a quiet moment (the pause menu, or a fade or wipe
  at least 90 % closed); without one, only after 120 s. Steps down need 1.5 s of a saturated GPU
  and go only as far as the load says; blocked rungs wait 60 s, doubling to 480 s. Each galaxy
  remembers its rung, applied during the load. On the host model of a console-like Observatory
  over 300 s: the old rules made 19 changes (2.67 s frozen), the calm rules 5, one of them in play;
  on a scene that alternates between 60 and 30 fps, 0 changes in play. The second console run
  (before the last round of rules) made 11 changes in 9 minutes.
- Video-memory gates on every step up (16 MiB margin, 32 MiB above 640x456), with an immediate
  drop to 640x456 below 12 MiB.

### HD textures (optional)

- 94 character and item textures (Mario and his forms, Luigi, Peach, Rosalina, Lumas, Toads,
  Bowser, Bowser Jr., Power Stars, launch and sling stars, power-ups) at 2x, made from the
  player's own game files with Real-ESRGAN (`realesr-general-x4v3`) and stored as DXT1/DXT5 with
  full mip chains: 4.5 MiB on the card. Built by [tools/hdtex](../tools/hdtex/README.md); never
  shipped.
- At run time a guest texture is hashed only on upload, and only when its format and size occur in
  the pack. Replaced textures keep the original's guest-visible size and sample their second
  level (the original's size) wherever they are not magnified. The pack is read whole at start-up
  (`GXV_HDTEX_PRELOAD`, 8 MiB), so no first use during play waits on the card.
- The builder pads borders by wrap-around only for textures that really tile, and a fidelity pass
  pulls the HD level back to the original wherever they differ by more than 4/255; the border
  check flags 0 of 94 textures (largest bias 3.6/255).

### Shader cache

- **Compiled shaders ship with the game.** The package carries a seed of 1,131 compiled shaders for the
  vitaGL in use (`shadercache/` in the VPK, copied to the card's cache at the first start). They are the
  port's own generated shaders, compiled with the Vita's shader compiler; they contain no game data.
  - 961 came from a console's own cache after tonight's play tests; 170 more come from a sweep of 100 galaxy and
    scenario entries on the desktop build (scripted play: triple jumps, spins, long jumps, Star Bits, pauses),
    compiled in Vita3K, whose output is byte-identical to the console's for the 961 both have.
  - Why it matters: on the console a shader compiled in play costs 200-400 ms (two of them took 846 ms in Good Egg
    Galaxy); from the cache it is a load of a few milliseconds.
  - The warm-up list (`shadercache/vgx_shaderlist.bin`, the programs linked behind the title) stays at the console's
    844 programs: listing all of the sweep's 1,012 would come close to vitaGL's 1,024-program table. The rest are
    linked when first drawn, from the cache.
- The vitaGL in use (upstream HEAD) keeps its cache in `v3`; the port's folder, the package's
  compiled-shader seed and the shader RAM pack follow it (`GXV_VGL_CACHE_DIR`).
- **Caches stay valid across updates:** an older cache layout is moved in place, in whole folders, before the graphics library starts.
### Controls and settings

- **Settings panel:** the pause menu's camera zoom and rear-touch setting are drawn as a panel in
  the game's own window style, message font and button pictures, in seven languages, with the
  game's menu sounds (`platform/src/port/settings_panel*.cpp`). The layout code is shared with an
  offline preview tool (`tools/settings_panel_preview/`).
- **Rear touch aiming** adds up slow slides below the pad's noise step, so a careful slide moves
  the pointer (before, slower than about 90 units a second stood still). Off by default; Triangle
  in the pause menu turns it on.

### From the Mario Strikers Charged port

- `DetachFromPool` skips the framebuffer in use; idle eviction never deletes the bound or in-use
  object.
- A step below today is undone only if the frames stayed about as long.
- vitaGL: uniform pool lifetime and blit flip, allocation recovery that waits for the collector,
  title ID buffer, shader cache and shader/program robustness, shader source length.

### vitaGL

Upstream vitaGL at `76f43e6` plus the patches and options in [tools/vitagl](../tools/vitagl/README.md):
`DEPTH_HINT` (per-scene depth load/store), `UNIFORM_PINGPONG` (two uniform arenas, no GPU drain at
a rollover), `ALIGNED_POOL`, `GPU_TIMERS`, `FBO_MSAA`, `DEPTH_SAMPLE`, `MASK_SKIP`, `ANISO_SWITCH`
(off unless asked for).

### Console test of the 1.0 code

A scripted 150 s route (walking, turning, jumps, triple jumps, spins, camera turns and two pauses), driven remotely on
a PS Vita through vitacompanion. First with the released 1.0 package itself (its basic log: frame rate, hitches over 100 ms
and picture size every 5 s; rear touch on, zoom 1.5×), then with the logging build of the 1.0 source for the A/B below:

| Scene (released 1.0 package) | Shown frame rate, 5 s windows | Hitches over 100 ms in play | Picture size |
| --- | --- | --- | --- |
| Good Egg Galaxy | 50.6 fps average, 40.8 lowest, 56.4 highest | 0 (longest frame 70 ms) | 640×456 throughout |
| Comet Observatory (early game) | 42.6 fps average, 31.5 lowest, 55.3 highest | 0 (longest frame 73 ms) | 640×456 throughout |

| Scene (logging build) | Shown frame rate | Picture-size changes in play |
| --- | --- | --- |
| Good Egg Galaxy | 49.4 fps average, 34.6 lowest 5 s window, 57 highest | 0 |
| Comet Observatory (early game) | 43.3 fps average, 33.1 lowest | 0 |
| Comet Observatory with `GXV_CHARAA=1` | 41.0 fps average, 30.3 lowest; the front end 0.66 ms a frame dearer | 0 |

That A/B is why the character edge smoothing ships off: the Observatory is limited by the front end and draws up to about 190
character draws a frame (the Lumas), and the gain on the picture is modest.
The run also showed what a missing shader costs on the console: two compiles in the middle of Good Egg took 846 ms. The
package's compiled-shader seed and warm-up list are why that should not happen to a player (see Shader cache).

### What has not been measured

Heavy boss fights (the Bowser's Galaxy Reactor finale) could not be reached by script and are unmeasured on the console.


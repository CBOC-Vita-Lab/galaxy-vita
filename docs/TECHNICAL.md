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
| `GXV_EDGE_AA` | 0 | Edge smoothing of the whole picture in the present (FXAA 3.11 console, in place of the blit or sharpened upscale; the HUD drawn again unfiltered; off on the multisampled rungs). `1` on, `2` shows the pixels it changes tinted red, `3` on for the left half of the screen only (A/B screenshots) |
| `GXV_PRESENT_FAST` | 0 | One-pass present (upscale + unsharp mask + edge smoothing) with non-dependent reads, half maths and no branch, in place of the sharpened upscale and of `GXV_EDGE_AA`'s FXAA program. `1`: 3 reads without edge smoothing (Strikers' FAST mask), `2`: 5 reads (today's mask exactly); with `GXV_EDGE_AA` both use a 5-read kernel that blends along horizontal / vertical edges (diagonals only at 960x544 1:1); the HUD drawn again plainly. `GXV_PRESENT_FAST_EDGE` / `_RAMP` (per mille, 100 / 150): where smoothing starts / is full |

Developer switches (`GXV_*` in `platform/vgx/src/` and `platform/vgx/vita/vgx_host_galaxy_vita.cpp`,
`PETARI_*` in `platform/src/`) are documented where they are read. Many have a verify mode
(`=2`) that runs the fast path and the old path side by side and logs every difference:
`GXV_STATELIST`, `GXV_RSCACHE`, `GXV_REGSHADOW`, `GXV_TEXMEMO`, `GXV_MERGEFAST`, `GXV_SPLIT`.

The automatic quality's ladder (rungs 0-8): 544x388, 592x422, 640x456 (the Wii's EFB size and the
start), 720x480, 800x504, 880x520, 960x544, then 960x544 with the EFB multisampled 2x and 4x.

## Settings in the game (3.0)

The package has no LiveArea Settings tile (`template.xml` has no `liveitem`, content-rev 4; `settings.png` stays in the tree but is not in the VPK; `tools/handheld/check_livearea.py` requires the tile to be absent). Settings open inside the game: a **Settings** choice on the Single Player / Multiplayer screen and **△ Settings** in the pause menu. An updated console keeps the old tile (the Vita does not refresh LiveArea); its `psla:-settings` request opens the in-game Settings. Rows that were removed are forced to their defaults for the run (file lines kept) and env.txt still wins: [TESTER-SWITCHES.md](TESTER-SWITCHES.md). Safe saves is always on (`SMG_ATOMIC_SAVES=0` still turns it off for a test).

## Version 2.3 Picture Modes (modes26)

Settings > Display > **Picture mode** has two choices; the older Quality / Performance / Locked 30 rows, Preset, Dynamic resolution, Sharper when possible,
Prioritize Mario, Anti-aliasing and Effects quality are no longer shown (their keys stay in `player-settings.cfg` and are read and written as before; the
mode ignores them). Settings maps the choice to start-up switches (`platform/src/settings/settings_launch.cpp`); an `env.txt` value wins and is listed as an override.

| | Performance (60), the default | Quality (30) |
| --- | --- | --- |
| Settings text | "60 fps. The smoothest play. Some far detail and effects are lighter." | "30 fps, smooth, with the highest quality. Always." |
| File | `fps_limit=0` `perf_mode=1` | `fps_limit=30` `perf_mode=0` |
| `GXV_PICTURE_MODE` | 60 | 30 |
| Frame rate | `GXV_FPS_LIMIT` unset (= 0), `GXV_AUTOQ_TARGET=60`, `GXV_STEADY30=0` | `GXV_FPS_LIMIT=30` (the driver's fixed cap: an even 30), `GXV_AUTOQ_TARGET=30`, `GXV_STEADY30=0` |
| Picture size | `GXV_AUTOQ_MIN=6` `GXV_AUTOQ_MAX=6` `GXV_AUTOQ_MSAA=0`: 960x544 only | the same, plus `GXV_AUTOQ_LOCKED30_FLOOR=6` `_START=6` |
| Edge smoothing | `GXV_EDGE_AA_NATIVE=0` (no pass over a 1:1 picture, about 3.5 ms of GPU), `GXV_AUTOQ_EDGESTEP=1` | `GXV_EDGE_AA_NATIVE=1`, `GXV_AUTOQ_EDGESTEP=0`, `GXV_PACE23_LOCKED_EDGE=0`: kept in every picture |
| Load steps | `GXV_AUTOQ_FXSTEP` as the start-up sets it (3) | `GXV_AUTOQ_FXSTEP=0` |
| Visual budget | `GXV_PERF_BUDGET=1` | `GXV_PERF_BUDGET=0` |
| `GXV_SMOOTH60` ladder | off unless `env.txt` turns it on | never runs |
| Stage table | the stage's row as written | exact keys only; frame forced to 960x544 fixed, edge pass on, effect steps held, no half-rate captures; plus the `mode=30` rows |

Old files migrate when read (the file on the card is rewritten only when the player saves): `perf_mode=1` and `fps_limit=0` read Performance; `fps_limit=30`,
`steady30`, and a file with `fps_limit=0` and no `perf_mode` or `perf_mode=0` (the old Quality) read Quality; any unknown `fps_limit` or `perf_mode` value reads Performance.

**The stage table by mode.** `platform/include/port/stage_knob.h` `Desc::ex` classes every stage-settable key: `x` exact (a bit-identical picture and game, applies in
both modes: `indcap_skip`, `scenes_dry`, `clearq`, `mirror_census`, `clip_skipempty`), `p` exact up to a value (`lightvol` 3, `mirror` 1, `colshadow_clip` 1), `s` a sacrifice
(everything else, including every key nobody has proven exact; `hsr_ties` too, as only 4 is exact). The decision follows `new/policy26/LEDGER.tsv` ("player loses: nothing
(exact)" / "bit-identical") and the older class letter E. A row without a tag is a 60-mode row; `mode=30` on a row (`Stage mode=30 ...`, `* mode=30 ...`) makes it a 30-mode row,
read only in the 30 mode and applied whole after the stage's exact keys. In the 30 mode `Resolve` (`platform/vgx/src/vgx_stagepreset.h`) reads the stage's 60 row, drops every
sacrifice key and its frame, applies the stage's `mode=30` row (else the `* mode=30` row), and fixes the frame. Every stage load logs one line:
`mode: 60|30, stage <name>[:scenario], preset keys applied: <switch>=<value> ..., skipped for mode: <key> ... frame` (in `log.txt`; with `GXV_STAGE_PRESETS=0` it says nothing was applied).

**The third class, `b` barely noticeable (q30-26).** The Quality (30) mode keeps `x`, `p` up to its exact value, and `b`: a cut a player would not see in normal play, kept so that the locked 30
has headroom while the picture stays at its best. A `b` key with levels has a range (`Desc::bMin` / `bMax`); Quality clamps a stage row's value into it instead of dropping the key (a `p` key
is lowered to its exact value: `lightvol=7` becomes 3). The umbrellas have a Quality level, "q", chosen when the mode is known (`platform/include/port/quality30.h`; `GXV_CPULOD`,
`GXV_ACROSS`, `GXV_GLOW26`, `GXV_FE_GOV`, `GXV_GAME_GOV` at any level mean q in Quality): cpulod q = the far-model material refresh and the far small-character / marked-family pose every second turn
beyond 3000 units (`cpulod.h` `Member::q`; star bits and coins shielded by `GXV_PROTECT_SPRITES=1`), across q = `INDCAP_SKIP`, `CLEARQ` and the silhouette gate confirmed by the occlusion query
(`across.h`), glow26 q = `GXV_IND_FLAT=1` and the exact light-volume cull (`glow26.h` `kQ`). The two governors run in Quality at level 1 only and aim at the 30 fps budget (front end 30.9 ms,
game thread 25.0 ms for the drawn turn, `fegov.h`); the far-draw governor (`GXV_CPUGOV`) stays off. The `mode:` line now reads
`mode: 30, stage <name>, preset keys applied: ..., clamped for Quality: <alias>=<asked>><kept> ..., skipped for mode: ...`; the `quality30:` line says what the q level rewrote.

## Version 2.3 Switches

Every 2.3 change is behind a switch in `ux0:data/smg/env.txt` (defaults are set in `vita/vgx-game/entry.cpp` or where the switch
is read). "perf" means on in Performance mode only (`GXV_AUTOQ_TARGET=60` without `GXV_FPS_LIMIT=30`); `=2` is a verify or
census mode where one exists (the fast path is checked against the original and differences are logged).

**On by default** (`NAME=0` gives the 2.2.1 / r110 behaviour, for comparison only):

| Switch | Default | What it does |
| --- | --- | --- |
| `GXV_DL_LAZY_KEY` | 1 | The Bee Mushroom crash: a deferred material-list refresh runs only while Mario's model manager still points at the models it was asked for (his one manager switches between his bodies) |
| `GXV_DL_GUARD` | 1 | A differed display list longer than its buffer is not stored (`FATAL: display list overflow` line) instead of overwriting the heap |
| `GXV_RT_DEFER` | 4 | The Comet Observatory crash: a texture the GPU may draw into is deleted this many presents after the back end drops it, and a copy texture that changes size gets a new name. vitaGL frees an unsampled texture's storage at once |
| `GXV_DRAWDONE_OTHER` | 1 | A draw-done wait made by the loading thread waits for its own finish (by count) and no longer takes the game thread's deferred wait, which could starve it at 60 pictures a second |
| `GXV_FOG_BIND` | 2 | The vertex-fog varying `v_fogt7` on TEXCOORD7 (`GXV_FOG_SLOT`); `1` = `v_fogc` on FOG (check with `GXV_FOG_SELFTEST=1`); `0` = the unbound `v_fog` of 2.0-2.2 |
| `GXV_ZOOM_LOGIC_VIEW` | 1 | Object clipping and every `isJudgedToClipFrustum` test use the game's own field of view, not the handheld zoom's narrower one |
| `GXV_ZOOM_DRAWCULL` | 1 | An actor kept alive by the above is still not drawn while it is outside the zoomed picture |
| `GXV_ZOOM_PULLSTAR` | 1 | (minor change) the zoom eases to 1.0 while a Pull Star is in reach |
| `GXV_ZOOM_FIXEDCAM` | 1 | (minor change) the zoom eases to 1.0 while the camera is a fixed point shot; `2` also for fixed-eye cameras that turn |
| `GXV_SETTINGS_POLL_MS` / `GXV_SETTINGS_EVENTS` | 250 / 2 | The Settings request (from a stale LiveArea tile) is polled every 250 ms (was 1 s), and system events are read: a resume or parameter notice checks for the request at once, a quit request is held for `GXV_SETTINGS_QUIT_MS` (750) and honoured only if no Settings request comes with it; `2` (the default since 2.3) logs the first 40 events, `0` reads none |
| `GXV_COLSHADOW_STABLE` | 1 | Mario's ground shadow: a stable choice of ground triangles (a band on the camera and facing thresholds, nearest to the drop point at the cap, a box radius that never shrinks below the radius it last grew to). `0` is the game's selection, `2` also lifts the polygons 10 units. `GXV_COLSHADOW_LOG=1` writes a `colshadow:` line every 300 pictures that ends in a verdict |
| `GXV_RUNG_SKIPRS_SETTLE` | 300 | Presents after a destroyed scene (a stage load) in which a picture-size change does not skip the resample of "rewritten every picture" textures; the evidence is forgotten at the load. `0` = off |
| `GXV_PAD_RAWFALLBACK` | 1 | When the Settings menu's pad read fails, the plain read of the Vita's own controls is used (logged once as `pad:`). `0` = off |
| `GXV_ABORT_TRAP` | 1 | `abort()`, `std::terminate` and `sceKernelExitProcess(!=0)` write a `FATAL:` line to the basic log and stop with a core dump |
| `GXV_SENSOR_BP` | 1 | Sensor hit checks: a sorted broad phase with a proven margin, same hit lists in the same order (`2` verifies against the plain loops) |
| `GXV_UPD_PREFETCH` | 1 | Cache hints in the update's pointer-chasing loops; nothing read for a result changes |
| `GXV_TITLEART_LIVEAREA` | 1 | The captured title art is also written to `ur0:appmeta/<ID>/livearea/contents` when that folder exists and the console allows it |
| `GXV_PERF_BUDGET` | perf | (minor changes) Performance mode's visual budget, parts `GXV_PB_LOD` 0.6, `_DECOR` 5000, `_PTCL` 0.5, `_SHADOW` 2800, `_ANIM` 2000, `_FUR` 2500 (each `0` = that part off); never near the player, no gameplay change. One `perf-budget:` line per stage load |
| `GXV_FLUSH_OFFSCREEN` / `GXV_PLANT_DRAWCULL` | perf | Flower-group plants wholly outside the picture are not posed / not drawn (exact) |
| `GXV_HR_PLANTMOVE` | perf | (minor change) far resting flower-group members move every second turn, their clock replayed |
| `GXV_SHADERPACK` / `GXV_SHADERPACK_SEED` | 1 / 1 | The RAM shader pack (`stages.pack`, one read at start-up) answers vitaGL's compiled-shader lookups; with no pack on the card the package's `app0:shadercache/<key>/stages.pack` is used. The package's pack must be built from the same seed it ships with (`tools/vita_shader_pack.cpp`) |
| `GXV_RUNG_RSORDER` / `GXV_RUNG_PREMULTI` / `GXV_RUNG_RSFREE` | 1 | A picture-size change resamples into premade textures first, has one premade texture per resampled texture, and never resamples through a pooled object (no GPU wait mid-change) |
| `GXV_STAGEWARM_REBUILD` | 1 | A warm-up queue made before its shaders were registered (the start-up menu list) is made again |
| `GXV_EFBSWAP2` | 1 | The shadow-alpha and indirect screen captures without an EFB scene split (r110; exact) |
| `GXV_FLUSH_PREFETCH` | 1 | Prefetch in the drawn turn's flushes (r110) |

Also back on from 2.2 (r108; 2.2.1 ran the 2.1.1 code): the stage warm-up hold (`GXV_STAGEWARM_HOLD_MS` 5000 and the
`GXV_STAGEWARM_*` family), the prebuilt picture sizes (`GXV_RUNG_PREBUILD`, `GXV_RUNG_SKIPRS`, both 1) and `GXV_AUTOQ_FAST60` (1).

**Available, off by default** (in the source for console A/B; set `1`, or `2` to verify first):

| Area | Switches |
| --- | --- |
| Front end (core 2) | `GXV_DL_VARIANTS` (8; 24 makes fur layers cache hits), `GXV_RETAINED`, `GXV_DIRECT_J3D`, `GXV_FE_PARALLEL` (vertex decode on core 3), `GXV_TEX_SWIZZLE`, `GXV_FE_CENSUS`, `GXV_STATIC_INST` (instanced plants in one draw) |
| Shaders (new texts: the pack needs a refill) | `GXV_TEVIN_CANON`, `GXV_FS_FINCLAMP`, `GXV_FS_ADEAD`, `GXV_FS_PROJ`, `GXV_IND_PRECOMP` (minor change) |
| GPU scenes and vertices | `GXV_INDCAP_SKIP`, `PETARI_BLOOM_SKIP`, `GXV_LA88_MIPS`, `GXV_SHADOWFETCH` (needs vitaGL vp24; minor change at shadow edges), `GXV_SHADOW_STENCIL` (pass24; exact on the host, default 0 until the console A/B: the shadow volumes are counted in the EFB's stencil and the silhouette fill drawn where it is not 0, so the whole-EFB alpha capture and its EFB scene split are left out; 2 = analyse and count only), `GXV_STATICCULL_PAL`, `GXV_DYNCULL`, `GXV_DLVCACHE`; half-rate (minor changes): `GXV_INDCAP_HALFRATE`, `PETARI_BLOOM_HALFRATE`, `PETARI_SHADOWTEX_HALFRATE` |
| Game thread | `GXV_PAR_PTCL` (+ `GXV_PAR_PTCL_OVERLAP`, already 1), `GXV_PTCL_LOOKPAR`, `GXV_JPA_LOOK_PURE`, `GXV_PAR_ANIM_CB2`, `GXV_PAR_LATE=2`, `GXV_PAR_PLANT`, `GXV_LIGHT_DEDUP`, `GXV_OC_LIGHTFIND`, `GXV_OC_CLIPFAST`, `GXV_ALOD` (minor change: far actors' update every second turn) |
| Pacing and picture | `GXV_PACE23`, `GXV_PACE60`, `GXV_UPSCALE` (`3` = Performance only), `GXV_UPSCALE_NEAR` |
| Boss fights (minor changes) | `PETARI_BOSS_PTCL`, `PETARI_BOSS_PTCL_GROW`, `PETARI_BOSS_HAZE`, `PETARI_BOSS_PSHADOW` |

Each one's exact behaviour is in the comment where entry.cpp sets it.

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
  kept up to date by the register writers, maps to the shader ids through a memo (4-way, 2,048
  sets; `GXV_KEYMEMO_WAYS`, 1 = direct-mapped; the first version had 1,024 direct-mapped entries and
  missed 7.4% of its lookups in the Comet Observatory, 4.3% in Good Egg, now 0.01% and 0.03% over a
  whole desktop run); render state, texture bindings and uniform blocks are rebuilt only from what
  they depend on.
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
  in-process verifier (`GXV_PASSVERIFY`) checks each change frame by frame. `GXV_EFBSWAP2` extends
  the swap to the two whole-screen captures every game picture makes with a format conversion (the
  shadow-volume / silhouette alpha capture, A8, and the indirect-effect capture, RGB565) when the
  frame's shaders read only channels the conversion leaves unchanged; a later frame that reads
  another channel gets the buffer converted first. Two scenes fewer per picture
  (`tools/test_vgx_efbswap2_gl.sh`).
- Reduced-size additive effects (`GXV_FXHALF`) are in the source but **off** by default. Half-size blur passes (`GXV_PASSHALF`)
  are on for the bloom chain only (`GXV_PASSHALF=2`, the default since gpu960; `0` = full size, `1` = every blur pass).
- **Asynchronous shaders** (`GXV_ASYNC_SHADERS`, default **off** since 2.1: the r101 console A/B showed cached links waiting behind the
  worker's compile lock; `1` turns it on, `0` = the blocking behaviour, exactly). A draw whose program is not
  linked and whose shaders are in neither the card's cache nor the package's seed does not compile on the render thread: the pair goes to one worker
  thread (`platform/vgx/src/vgx_async_shaders.h`; cores 1-2 at the lowest priority, never the fourth (system) core: hitch33 item 11 refuses to run the
  worker if that core is in its mask; an earlier core-3 worker powered a console off) and the draw is
  skipped, as a draw of a program that failed to link always was, until the worker has written vitaGL's cache files (`vglCompileShaderPairToCache`,
  `tools/vitagl/patches/0020`; no GL object, no GXM call off the render thread). The program then links with a file read. Never skipped: the characters'
  draws (the HD pack's list), the HUD and every draw without depth test, draws into textures, opaque cached geometry (the map). A skipped draw goes on to
  the blocking path after `GXV_ASYNC_SHADERS_MAXWAIT_MS` (15,000). `GXV_ASYNC_SMALL_VERTS=<n>` lets opaque cached draws of at most n vertices wait too.
  The basic log gets `async shaders: on, worker ...` once and an `async-shaders-5s:` line when the worker or a skipped draw had work in the window.
  Host tests: `tools/test_vgx_async_shaders.sh`.
  The worker compiles only while the C heap has `GXV_ASYNC_SHADERS_HEAP_MIB` (8) free, and (asyncsh31) every vitaGL allocation it makes comes from newlib's
  heap only (`vita/vgx-game/alloc_wrap.cpp`, link wrappers of vglMalloc / vglCalloc / vglMemalign / vglRealloc; `platform/vgx/src/vgx_worker_alloc.h`): a block the
  heap cannot give fails the job over to the render thread instead of reaching vitaGL's unlocked pools. Host test: `tools/test_vgx_worker_alloc.sh`.
- **Ubershader census** (`GXV_UBERSHADER`, default **0**; asyncsh31, stage 1 of `reports/asyncsh31.md`): `1` = count only, nothing is drawn differently. One
  `first-use: ubershader census:` line per stage left: the draws in play whose program needed the compiler, by whether the fragment ubershader
  (`platform/vgx/src/vgx_ubershader.{h,cpp}`: one program per varying/sampler interface that reads the key's TEV configuration from `u_ucfg`) would have drawn
  them meanwhile, and the compiles that would still block. Host proof: `tools/test_vgx_ubershader_gl.sh` (ubershader against each key's own text on Mesa).
- Half precision (fp16) fragment shaders (`GXV_FS_HALF`, **off** by default). `1`: only the shaders
  `FsHalfAnalyse` proves to differ from the float shader by at most one 8-bit level (a forward error
  analysis of the TEV stages: unit-range inputs, a half rounding after every operation; no indirect
  stages, fog, compare-mode stages, wrapped final values or registers outside 0..255); their colour
  variables and the `u_tev` array are `half`, texture coordinates and the alpha test stay float.
  `3`: the same with a bound of half a level. `2`: every shader through vitaGL patch 0012 (A/B only).
  Host test: `tools/test_vgx_fshalf.sh`. Turning it on changes the text of the eligible shaders, so the
  shipped shader seed has to be regenerated.
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
  - The per-galaxy list (`shadercache/vgx_stagewarm.bin`, same format as the card's `ux0:data/smg/shadercache/vgx_stagewarm.bin`)
    is read from the package too, only when the package holds the seed folder of this build's key; the card's records
    win, the others are added and written to the card's file at the next quiet moment (`GXV_STAGEWARM_PKG`, default 1).
    While a galaxy loads, its programs may take up to `GXV_STAGEWARM_COMPILE_MS` (default 1500) ms beyond the usual slices.
- **The vertex-fog varying has a fixed register (2.3, `GXV_FOG_BIND`, default 2).** Before 2.3 `v_fog` had no semantic binding,
  so vitaGL gave it the first free TEXCOORD of whichever shader met it first in that run; vertex and fragment shaders compiled in
  different runs (card, package, emulator) could disagree, and the fog was lost or a texture coordinate read the fog. Now the
  varying is `v_fogt7` on TEXCOORD7 (`GXV_FOG_SLOT`, free in every fog shader of the 2.2 list), or `v_fogc` on FOG with
  `GXV_FOG_BIND=1` (`GXV_FOG_SELFTEST=1` logs whether the console's compiler takes it); `0` is the old unbound `v_fog`. The new
  name changes only the ~210 fog shader texts, so their old compiled files are never found again (vitaGL's cache is keyed by the
  text), while the cache folder key, every other packaged shader and the warm-up lists stay valid.
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


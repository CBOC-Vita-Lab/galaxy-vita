# Tester switches for the removed Settings rows

Settings in the game no longer show these rows. A settings file from 2.3 keeps the values it has for them (the 2.3 tuning is not changed);
a new file uses the value in the second column. A line in `ux0:data/smg/env.txt` still forces the old behaviour, and an `env.txt` value wins
over the file. These are for testers; players should not need them.

Each line is `NAME=value` in `ux0:data/smg/env.txt`, one per line. Restart the game after changing the file.

| Removed row | Automatic value | `env.txt` lines that force the old choice |
|---|---|---|
| Game speed: Smooth (slowdown allowed) | Full speed | `GXV_SLIP=1` and `PETARI_FRAMESKIP=3` (together) |
| Sharpness | Follows the upscale (`auto`) | `GXV_SHARPEN=off`, or a number such as `GXV_SHARPEN=0.5` (0 to 1.5) |
| HD textures | On when the HD pack is installed | `GXV_HDTEX=0` |
| Depth of field | On | `GXV_DOF=0` |
| Core plan: Off | Every part on the fourth core, when the console allows | `GXV_CORE3_IO=0`, `GXV_CORE3=0`, `GXV_GC_CORE=2`, `GXV_AUDIO_THREAD_CORE=0` (all four make the old "off" plan) |
| Renderer: Standard | Direct renderer | `GXV_GXM_DIRECT=0` |
| Logic overlap: Off | On | `GXV_DRAWDONE_DEFER=1` |
| Material updates: every turn | Made once per picture | `GXV_DL_LAZY=0` |
| Far animation: Full | Simplified when far away | `GXV_ANIM_LOD=0` (off), or `GXV_ANIM_LOD_DIST=1000` (a nearer distance, in game units) |
| Safe saves: Off | Always on | `SMG_ATOMIC_SAVES=0` |
| The whole settings file | Read at start | `GXV_SETTINGS=0` (the file is ignored and Settings applies nothing at start-up) |

Notes:
- Every name above was checked against the code (`platform/src/settings/settings_launch.cpp` and the reader of each switch).
- `GXV_SLIP=1` allows slowdown; `PETARI_FRAMESKIP=3` lets up to 3 pictures be skipped in a row. The old "Smooth" speed used both.
- `GXV_CORE3_IO` and `GXV_CORE3` are read by `platform/include/port/core_layout.h`, `GXV_GC_CORE=2` by `platform/vgx/vita/vgx_host_galaxy_vita.cpp`
  (the collector runs on core 2 instead of the render core), `GXV_AUDIO_THREAD_CORE` by `vita/runtime/audio.cpp`.
- `GXV_ANIM_LOD_DIST` is read only when `GXV_ANIM_LOD` is not `0`; values below 500 are ignored. The default distance is 3000.
- Picture, 60 Assist and Language are still Settings rows. **60 Assist** (shown under Picture while Picture is Smooth Motion; key `assist60` in
  `player-settings.cfg`, absent = On) is read once at start-up: On keeps 2.3's estimated second picture (`GXV_HRW=5`), Off asks for `GXV_HRW=0`
  (only real pictures, the plain 60 target). A `GXV_HRW` line in `env.txt` wins and dims the row. `GXV_FPS_LIMIT=30` in `env.txt` with Smooth
  Motion still means the fixed 30 cap (`GXV_HRW=0`), as in 2.3. Other test switches are in [TECHNICAL.md](TECHNICAL.md#runtime-switches).

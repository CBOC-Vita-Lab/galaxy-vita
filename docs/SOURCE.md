# Source Record

## Version 1.2 (`galaxy-v1.2`)

Version 1.1 plus the changes below, built from this repository (the release commit) with the player build flags, the game code profile-guided (`tools/pgo/smg.profdata.gz`, recorded under the ARM harness) and vitaGL `nhustdx`: upstream `9bf3e3f` + patches 0001-0012 (0009 scene begin fails closed, 0010 checked depth/stencil, 0011 GXM state cache, 0012 optional fragment precision), `GPU_TIMERS`, `FBO_MSAA`, and the `DRAW_SPEEDHACK INDICES_SPEEDHACK SAMPLERS_SPEEDHACK` options (each checked against the vgx back end; tools/vitagl/README.md).
- **Undoing 1.1's slowdown:** shader generator version back to 2 (the 1.0 compiled-shader cache key `c0477cd7`); the eye fix wraps alpha only (`GXV_TEVWRAP=2`); a fixed 30 ceiling instead of the 30/20 pacer; the Settings parameter polled once a second.
- **Frame rate:** the 30 FPS floor in every picture rate (`GXV_AUTOQ_TARGET=30`), Mario-first degradation (`GXV_AUTOQ_FXSTEP=2`, `GXV_CHARAA_AUTO`), faster recovery (`GXV_AUTOQ_RECOVER`, 10 s), bounded slip instead of halving the picture rate (`GXV_SLIP`, cap 20 %), the queue depth judged by the back end's own CPU time under a cap (`GXV_FIXED_DEPTH`), `GXV_PACE_VBLANK_COUNT`, the game thread alone on core 0 (`GXV_CORE0_ALONE`), disc read-ahead and one movie decoder on the system core when allowed (`GXV_CORE3_IO`), DSP mixing on its own thread (`GXV_AUDIO_THREAD`), the end-of-turn draw-done wait deferred (`GXV_DRAWDONE_DEFER=1`), frame clock priority (`GXV_VBLANK_PRIO`).
- **Renderer work:** rigid-draw merging (`GXV_RIGIDMERGE`), small palette (`GXV_PALROWS`), merged light passes (`GXV_LTMERGE`), copy programs (`GXV_COPYVAR`), no-discard where the alpha test cannot fail (`GXV_NODISCARD`), a 4-way shader-key memo (`GXV_KEYMEMO_WAYS`), hardware JPEG for THP frames (`GXV_THPHW`), `-fno-math-errno`.
- **Picture fixes:** `GXV_FIX_TEVIN` (Lumas), `GXV_FIX_INDPREV`, `GXV_FIX_DSTA`, `GXV_FIX_LOGICOP`, `GXV_FIX_TEXOFF`, `GXV_FIX_LODBIAS`, `GXV_AUTHORED_I4_MIPS`, `GXV_FIX_BCKCNT` (Bowser's battle intros: the converter's signed key counts, repaired at load), `GXV_FIX_FURMAP` (Queen Bee's fur), the 1.1 repair guards (clip capture, dome zoom, zero destination alpha, EFB/render-target allocation and rebinding, display-list bounds, GPU timer ring, Settings save).
- **Gameplay and controls:** music after sleep or Home (`GXV_DVD_REOPEN`, `GXV_AUDIO_RETRY`), `GXV_TAP_CLEAN`, `GXV_SHAKE_LATCH`, PS TV pad defaults (`GXV_PAD_DEFAULTS`), mouse-style aiming, reduced motion in play (`GXV_REDUCED_MOTION`), Locked 30 / Up to 60.
- **No game artwork in the package:** the loading screen and the Settings backdrop (and the bubble and LiveArea pictures, where the console lets the game write its own folder; a test write decides) are made on the console from the player's own title screen, captured once (`GXV_TITLEART`, `platform/include/port/title_art.h`, `vita/vgx-game/title_art_runtime.cpp`; see [ASSETS.md](ASSETS.md)); the package's own pictures are neutral drawings and the loading screen's fallback is a plain starfield.
- **Settings menu:** Display page choices stored as optional keys of `player-settings.cfg` (`above_native`, `mario_first`, `anti_aliasing`, `picture_sharpness`, `hd_textures`, `effects_quality`, `depth_of_field`, `core_plan`) and mapped at start-up to existing switches (`GXV_AUTOQ_MAX`, `GXV_AUTOQ_MSAA`, `GXV_SHARPEN`, `GXV_HDTEX`, `GXV_AUTOQ_FXSTEP`, `GXV_CHARAA_AUTO`, `GXV_CORE3_IO`) and one new read-once switch, `GXV_DOF=0` (depth-of-field blur not drawn; default on).
- **Tilt settings:** Controls page rows Tilt sensitivity and Tilt with, stored as the optional keys `motion_level` (1 to 10, default 5; the percentages are 50, 63, 75, 88, 100, 120, 140, 160, 180, 200; a 1.2 beta file's `motion_gain` percentage is read as the nearest level and no longer written) and `motion_source` (`sensor` or `stick`) and mapped at start-up to the read-once switches `GXV_TILT_GAIN` (a percentage, 50 to 200, for env.txt; level 5 sets nothing) and `GXV_TILT_STICK` (env.txt wins and is reported as an override). The scaling (angle from the calibrated level, unit length kept, 100% bit-exact) is `platform/include/port/tilt_gain.h`, applied in `vita/runtime/input.cpp` to the motion sensor and to the left-stick tilt path; `platform/src/port/handheld_ui.cpp` words the tilt prompts for the stick when it is chosen.
- **Start-up:** bounded waits with a stall message, `GXV_BOOT_TITLE_FALLBACK=60`, named libshacccg and graphics-start errors.
- Half precision in the fragment shaders proven to stay within one 8-bit level of the float result (`GXV_FS_HALF=1`; `2` is every shader through vitaGL patch 0012, `0` off).
- Vertex fog on (`GXV_FOGVTX=2`). Test switches, off: `GXV_SANITIZE` (bit 16 counts a draw or copy that samples the target it writes; log only), `GXV_GLDEBUG` (GL debug output on the host), `GXV_SCENELOG_MIN`, `GXV_WAITLOG`, `GXV_RESUME_RESET`, `GXV_RIGIDMERGE_DLMAX`, `GXV_FXHALF`, `GXV_VITA_DISPLAY_SCENES` (default 1) / `FBO_SCENES` (default 8) / `TRIPLE` / `RTMEM_LOG`.
- **Found on the console (31 Oct 2026):**
  - Frame arenas reserved at their floors at start-up and grown in steps, never doubled (`platform/vgx/src/vgx_arena.h`, `GXV_ARENA_KIB`; host heap exhaustion after ~14 galaxies in one session).
  - The C heap's use, its growth steps and the heap-exhausted FATAL line in the player log every 30 s.
  - A step below 640x456 is judged on the GPU's busy time; while the 30 floor is missed any real saving (3 %) is kept, otherwise half the predicted saving; useless steps are undone and remembered per stage (`vgx_autoq`).
  - The controller's 60 target (`GXV_AUTOQ_TARGET=60`) is in the code but not offered in Settings in 1.2.
  - The packaged shader seed is read in place as a read-only second level behind the card's cache (no copy on the first start; `GXV_VITA_SEED=copy` restores the copy).
  - Title capture accepts the title camera's cyan planet horizon; no picture work when the console refuses the app and appmeta folders (a retail Vita does).
  - Hints whose Vita wording made a line too wide are wrapped again at spaces; env.txt lines up to 2 KiB.
- Compiled-shader seed: 2,144 programs under `c0477cd7`, compiled on a PS Vita by a tour of all 45 galaxies with the 1.2 shader generator, plus the learned warm-up list.

## Version 1.1 (`galaxy-v1.1`)

Version 1.0 plus the changes below, built from this repository (the release commit) with the 1.0 player build flags and vitaGL `nhustd` with patch `0008-no-present-submit.patch` (8 patches):
- `platform/vgx/src/vgx_hdtex.cpp`: textures whose name contains "eye" keep the game's own image even when the optional HD textures pack has one (`GXV_HDTEX_EYES=1` turns the pack's eyes back on for testing).
- `platform/vgx/src/vgx_shadergen.cpp`: final TEV colour and alpha wrap to a byte as on the Wii instead of clamping (`GXV_TEVWRAP`, on by default in 1.1); shader generator version 3.
- `platform/src/settings/`, `vita/vgx-game/settings_runtime.cpp`, `platform/src/input/control_bindings.cpp`: the Settings menu (LiveArea tile, in-game hold and return, restart), control remapping, fixed 30 FPS, language choice (`GXV_SETTINGS`, `GXV_REMAP`, on by default in 1.1).
- `platform/vgx/vita/`: truthful loading progress (`GXV_BOOTGUARD`, on by default in 1.1) and the title-screen loading picture (`vita/vgx-game/sce_sys/loading-title.rgba`, see [ASSETS.md](ASSETS.md)).
- `vita/vgx-game/fd_guard.cpp`: a seek on a file handle that is not open fails with EBADF instead of crashing.
- Basic log: loading stages, time to title, and shaders prepared per five-second line.
- Off by default, for testing only (`env.txt`): `GXV_PACE`, `GXV_AUTOQ_TARGET=30`, `GXV_DSPNEON`, `GXV_DSPFIR`, `GXV_DSPRAMP`, `GXV_FXSCANFAST`, `GXV_XFBURST`, `GXV_POSDEFAULT`, `GXV_TEVWRAP_ELIDE`, `GXV_BOOTDIALOG`, `GXV_BOOTDATA`, `GXV_CORE3_PROBE`, `GXV_CORE0_ALONE`, `GXV_CORE3_IO`, `GXV_PACE_VBLANK_COUNT`, `GXV_FIXED_DEPTH`, `GXV_WAITLOG`.

The tables below describe 1.0.

## Version 1.0 (`galaxy-v1.0`)

| Item | Value |
| --- | --- |
| Package | `Galaxy-Vita.vpk` |
| Title ID | `SMGV00001` |
| VPK SHA-256 | see `package.sha256` in [cboc-release.json](../cboc-release.json) |
| Executable SHA-256 | see `package.runtimeSha256` in [cboc-release.json](../cboc-release.json) |
| Source | "the 1.0 source": the port's development line at `56e8263a393a0242e604e9778c467468f28a9aeb` |
| Player build flags | `PORT_RUNTIME_DIAGNOSTICS=0 PORT_LOGGING=1 PORT_SILENT_STDIO=ON` (a basic log only) |
| Game code | ThinLTO: the 1,834 units as bitcode, `ld.lld -r --lto-O2 -mllvm -emulated-tls`, one archive |
| Renderer | `-O3` |
| vitaGL | `nhustd`: upstream `76f43e6` + [tools/vitagl](../tools/vitagl/README.md) (7 patches, `GPU_TIMERS`, `FBO_MSAA`; options `DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP`) |
| Toolchain | Clang / LLD 18.1.3, VitaSDK GCC 15.2.0 |

The executable is built from this tree with the commands in [Building](BUILDING.md). This
repository does not carry the development history: the 1.0 source was copied in from the port's
development line at the revision above. Machine-specific paths in comments and test scripts were
replaced by neutral ones (`/opt/vitasdk`, `/opt/galaxy-vgl`), the HD texture tools got relative
default folders, and the vitaGL scripts were rewritten to take their inputs as parameters; nothing
that goes into the executable changed. Development notes, work-in-progress plans and local
captures were left out.
[SOURCE-MANIFEST.json](SOURCE-MANIFEST.json) lists every source file with its hash;
`python3 -I tools/vgx_build/build.py check` compares the tree with it.

The vitaGL recipe was checked by rebuilding it from upstream with the scripts in `tools/vitagl`:
all 31 object files are byte-identical to the library the 1.0 source was developed and tested
with.

What is in this release is described in [Technical Notes](TECHNICAL.md#whats-in-version-10).

Verification: the host tests (`tests/`, `tools/test_*.sh`), Vita3K runs (start-up, Gateway
Galaxy, the Comet Observatory, shader-cache migration on update and on a fresh install), and the
ARM instruction-count harness for the performance work. Test builds from this line ran on a
PS Vita on 23 October 2026; the release build itself is checked on a console before it is
published (see [Known Issues](../KNOWN-ISSUES.md)).


# Source Record

## Version 1.1 (`galaxy-v1.1`)

Version 1.0 plus four changes, built from this repository at `95dbb73965f16ddb9854f1ad4e977e63c7251504` with the 1.0 player build flags:
- `platform/vgx/src/vgx_hdtex.cpp`: textures whose name contains "eye" keep the game's own image even when the optional HD textures pack has a replacement (`GXV_HDTEX_EYES=1` turns the pack's eyes back on for testing).
- `platform/vgx/vita/vgx_host_galaxy_vita.cpp`: the loading stages and the moment the title is reached go to the basic log; a start still loading after 90 s says so on screen.
- `vita/vgx-game/fd_guard.cpp`: a seek on a file handle that is not open fails with EBADF instead of crashing in the C library (a start-up crash seen once in a few launches); the basic log notes it.
- `platform/src/gx/vgx_galaxy.cpp`: the five-second line of the basic log adds the shaders prepared in that time.

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


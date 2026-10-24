# Building eboot.bin

Developer tools for building the game executable from this repository. Players don't need any of
this: the release VPK is ready to install.

## Requirements

- Linux with Python 3.9+, CMake 3.24+, Ninja, and Clang, `ld.lld` and `llvm-ar` 18 (the release used 18.1.3).
- VitaSDK with GCC 15.2.0 (`VITASDK`, default `/opt/vitasdk`).
- A `VGL_DEPS` folder: vitaGL and the rest of the link set. [tools/vitagl](../vitagl/README.md) builds it.

Use paths without spaces, colons or dollar signs. On a Mac, use a Linux container with the source
mounted at `/source`, the SDK at `/opt/vitasdk`, the dependencies at `/deps` and a writable `/build`.

## Build

From the repository root:

```sh
export VITASDK=/opt/vitasdk
export VGL_DEPS=/deps            # for example build/vitagl/nhustd
python3 -I tools/vgx_build/build.py check
python3 -I tools/vgx_build/build.py all --build-dir /build --jobs 4
```

`check` only reads files: it verifies every source file against
[docs/SOURCE-MANIFEST.json](../../docs/SOURCE-MANIFEST.json) and looks for the SDK and dependency files
listed in [inputs.json](inputs.json). `all` runs the stages in order:

| Stage | What it makes |
| --- | --- |
| `source` | the 1,834 game units (the Petari decompilation) with Clang: `vita-source/` and its compile database |
| `lto` | the same units as ThinLTO bitcode, linked by `ld.lld -r --lto-O2` into one object: `vita-source-lto/libgalaxy_vita_sources.a` (about 12 % less game-thread work) |
| `runtime` | the Vita runtime and the vgx renderer (renderer at `-O3`): `vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a` |
| `link` | `vita-vgx-game/eboot.bin`, linked with VitaSDK GCC against `VGL_DEPS` |

Each stage is incremental and keeps its compiler log under `<build>/logs`. The first `source` and
`lto` runs take a while (the game is about 1,800 files); after a change outside `decomp/`, only
`runtime` and `link` need to run again.

The default is the player build: diagnostics are compiled out and only a basic log is kept
(`PORT_RUNTIME_DIAGNOSTICS=0`, `PORT_LOGGING=1`, `PORT_SILENT_STDIO=ON`): `ux0:data/smg/log.txt` gets the frame rate
and hitches every 5 s, picture-size changes, settings and errors. `--logging` makes a test
build with the full diagnostic log (much more detail; never publish one). `--no-lto` links the plain game archive.
`--label` sets the build label the renderer reports (default `1.0`).

## Result

`<build>/vita-vgx-game/eboot.bin`. To compare a build with the hashes recorded for the release:

```sh
python3 -I tools/vgx_build/build.py verify --build-dir /build
```

The CMake target in `vita/vgx-game` also makes a plain `smg-vgx.vpk` (title ID `SMGV00001`) when its
LiveArea pictures are present; see [docs/BUILDING.md](../../docs/BUILDING.md#packaging).

## Safety

The driver never changes the source, the SDK or the dependency folder. It refuses a build directory
inside the source tree (other than `build-vgx`), output symlinks (except the case shim's read-only
header aliases) and paths with spaces. It creates no VPK on its own and never talks to a Vita.

The runtime generator, report and include-case helper (`vita_runtime_gen_vgx.py`,
`vita_runtime_report_vgx.py`, `make_case_shim.py`) are the port's own build helpers.

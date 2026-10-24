# Building

How to build the game executable and the VPK from this source. Players don't need any of this.

## 1. vitaGL

The port links a patched vitaGL. [tools/vitagl](../tools/vitagl/README.md) builds it from upstream
and gives a complete `VGL_DEPS` folder:

```sh
VITASDK=/opt/vitasdk VGL_UPSTREAM=vitagl-upstream VGL_LIBS=/path/to/vita-libs \
  tools/vitagl/build-release.sh          # -> build/vitagl/nhustd
```

## 2. eboot.bin

[tools/vgx_build](../tools/vgx_build/README.md) builds the game (ThinLTO), the runtime and renderer,
and links `eboot.bin`:

```sh
export VITASDK=/opt/vitasdk VGL_DEPS=$PWD/build/vitagl/nhustd
python3 -I tools/vgx_build/build.py check
python3 -I tools/vgx_build/build.py all --build-dir /build --jobs 4
```

The result is `/build/vita-vgx-game/eboot.bin`, the player build (no diagnostics or log file).
Toolchain: Clang, `ld.lld` and `llvm-ar` 18.1.3 for the game and runtime, VitaSDK GCC 15.2.0 for
the link. On a case-sensitive file system the driver adds a small include shim for the dozen
`#include` lines whose case differs from the file name; nothing in `decomp/` is edited.

## Packaging

The release VPK (`Galaxy-Vita.vpk`, title ID `SMGV00001`) holds:

| Member | Where it comes from |
| --- | --- |
| `eboot.bin` | the build above |
| `sce_sys/param.sfo`, `sce_sys/livearea/contents/template.xml` | `vita/vgx-game` (CMake, `vita_create_vpk`) |
| `sce_sys/icon0.png`, `pic0.png`, `livearea/contents/bg.png`, `startup.png` | `vita/vgx-game/sce_sys/`, drawn by `tools/handheld/make_livearea.py` (original art) |
| `shadercache/` | the compiled-shader seed: the shaders the port generates, compiled once with the Vita's own compiler, so the first launch has less to do |

`cmake --build /build/vita-vgx-game` makes `smg-vgx.vpk` from the first three. The release adds
the seed to it. A VPK without the seed works too; the console then compiles the shaders as the game
meets them and keeps them in `ux0:data/smg/shadercache/`.

## Developer Checks

- `python3 -m unittest discover -s tests`: release files, checksums, the build driver, the cooker
  layout and the install checker. These run anywhere with Python 3.
- `tools/test_*.sh`: host suites for the renderer, the automatic quality, the front-end split,
  the HD texture cache, the shader cache, the scheduler and more. Each script says what it needs
  in its header; several build against the VitaSDK headers or a matching vitaGL folder
  (`VITASDK`, `VGL_DEPS`), and a few run in an ARM container image.

None of these checks touch a Vita or game data. Gameplay checks happen on a console.

## Layout

| Path | What it is |
| --- | --- |
| `decomp/` | the Petari decompilation (game, JSystem, NW4R, RVL SDK) |
| `platform/` | the Wii SDK replacement, the GX layer and the vgx renderer (`platform/vgx/`) |
| `vita/runtime/` | Vita input, audio, memory and handheld controls |
| `vita/vgx-game/` | the `SMGV00001` link and package target |
| `vita/CMakeLists.txt` | the game-source build (`galaxy_vita_sources`) |
| `tools/cook/` | the game-data converter |
| `tools/vgx_build/`, `tools/vitagl/` | the build driver and the vitaGL recipe |
| `tools/hdtex/` | the optional HD texture pack builder |
| `tests/` | host tests |

`platform/desktop/`, `platform/android/`, `platform/src/xr/`, `armhf/` and the root `CMakeLists.txt`
are the desktop, Android/VR and ARM-harness builds the port grew from. They stay as reference
source and are not part of the Vita release.

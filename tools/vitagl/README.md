# vitaGL for the Galaxy port

The port draws through [vitaGL](https://github.com/Rinnegatamante/vitaGL). Version 1.0 links a vitaGL
built from upstream plus a few patches and options. This folder holds everything needed to make
that exact library.

## Recipe (variant `nhustd`)

1. **Upstream** vitaGL at commit `76f43e6fbe1513cee44118da7ff2c16728c4fed2`.
2. **[patches/](patches/)**, applied in order: fixes first made for the Mario Strikers Charged port
   (uniform pool lifetime and blit flip, allocation recovery, title ID buffer, shader cache and
   shader/program robustness, shader source length) and the shader cache file names the port's
   compiled-shader seed uses.
3. **[gpu_timers.py](gpu_timers.py)**: `GPU_TIMERS`, the per-scene GPU time the automatic picture
   quality reads, and `ANISO_SWITCH` (off unless the game asks for it).
4. **[fbo_msaa.py](fbo_msaa.py)**: `FBO_MSAA`, multisampling per framebuffer object, for the automatic
   anti-aliasing.
5. **[build-vitagl.sh](build-vitagl.sh)** with the options `DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL
   GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP` on top of the base set
   (`HAVE_SHADER_CACHE HAVE_SHARK_LOG LOG_ERRORS STORE_DEPTH_STENCIL SKIP_SPLASHSCREEN
   STRICT_DRAW_COMPLIANCE`, `-O3 -ffast-math`, Cortex-A9 NEON, hard float). `ALIGNED_POOL`, `DEPTH_HINT`,
   `UNIFORM_PINGPONG`, `DEPTH_SAMPLE` and `MASK_SKIP` are source patches the script applies to its own
   copy; each is described where it is applied.

[build-release.sh](build-release.sh) runs all five steps:

```sh
git clone https://github.com/Rinnegatamante/vitaGL vitagl-upstream
VITASDK=/opt/vitasdk VGL_UPSTREAM=vitagl-upstream VGL_LIBS=/path/to/vita-libs \
  tools/vitagl/build-release.sh            # -> build/vitagl/nhustd
```

`VGL_LIBS` is a folder with `lib/` and `include/` holding the rest of the link set:
vitaShaRK (`libvitashark.a`, `vitashark.h`), SceShaccCgExt (`libSceShaccCgExt.a`, `shacccg_ext.h`),
math-neon (`libmathneon.a`, `math_neon.h`) and the taiHEN and kubridge stubs (`libtaihen_stub.a`,
`taihen.h`, `libkubridge_stub.a`, `kubridge.h`). They come from the VitaSDK packages of the same
names. The script copies them next to the new `libvitaGL.a`, so `build/vitagl/nhustd` is a complete
`VGL_DEPS` folder for [the game build](../vgx_build/README.md).

With VitaSDK GCC 15.2.0 this recipe gives object files byte-identical to those of the library the
1.0 source was built and tested with (31 of 31 compared; the archive itself differs only in member
timestamps).

## Licence

vitaGL is GPL-3.0 ([licenses/GPL-3.0.txt](../../licenses/GPL-3.0.txt)); the patches and scripts here
are offered under the same licence.

# vitaGL for the Galaxy port

The port draws through [vitaGL](https://github.com/Rinnegatamante/vitaGL). Version 1.0 links a vitaGL
built from upstream plus a few patches and options. This folder holds everything needed to make
that exact library.

## Recipe (variant `nhustd`)

1. **Upstream** vitaGL at commit `9a14d17808bdeb5c74648e8e1221093df3bfc83f` (30 Oct 2026: faster matrix and uniform paths, small-transpose and index-conversion speed-ups; versions 1.0 and 1.1 used `76f43e6`).
2. **[patches/](patches/)**, applied in order: fixes first made for the Mario Strikers Charged port
   (uniform pool lifetime and blit flip, allocation recovery, title ID buffer, shader cache and
   shader/program robustness, shader source length) and the shader cache file names the port's
   compiled-shader seed uses, then the two fail-closed fixes for the console freezes: `0009` (a scene that
   cannot begin is reported by `scene_reset`, every submitting caller stops, no `sceGxmEndScene` follows,
   bounded retry then fail-stop) and `0010` (depth/stencil creation returns a checked result, frees only its
   own unsubmitted partial allocations and never publishes an incomplete surface). Host tests:
   `tools/test_vitagl_scene_fail.sh --vitagl-source <patched tree>`.
   `0013` (the direct draw, vglDrawElementsDirect) and `0014` (the vgx draw caches, off unless the back end turns them on with
   `vglSetDrawCaches`: each program keeps the fragment programs the shader patcher made for it by blend and target mode, and a
   program whose uniforms did not change points sceGxm at the uniform-ring copy of its last upload instead of copying its default
   uniform buffers again after a program switch; `GXV_VGL_FRAGMEMO`, `GXV_VGL_UNIREUSE`, default on). Host test of 0014:
   `tools/test_vgl_drawcache.sh --vitagl-source <patched tree>`.
   `0015` (vgx uniform shadows, nothing changes unless called: `vglGetUniformShadow` gives where a program keeps an F32 vec4 /
   vec4 array / mat4 uniform, `vglUniformShadowWritten` sets the dirty flags and forgets the ring copy as `glUniform*` does; the
   back end's `GXV_UNI_SHADOW=1`, default off). Host test of the back end's use: `tests/vgx_uni_shadow.cpp`.
   `0018` (`MASK_KEEP`, needs the `DEPTH_HINT` and `MASK_SKIP` options): a framebuffer-object scene that does not load its
   depth/stencil surface and begins with the scissor test off starts with the surface's background mask set, and the full-target
   quad `update_scissor_test` draws to set the mask everywhere is left out (`vglSetMaskKeep(GL_TRUE)`, the host's `GXV_VGL_MASKKEEP=1`,
   default off; while off the background mask stays clear, as `MASK_SKIP` assumes; `vglGetMaskKeeps()` counts the quads left out).
   Its gxm.c part is the depth/stencil argument of the framebuffer object's `sceGxmBeginScene` (after `apply_depth_hint`, which
   build-vitagl.sh inserts), so the patch applies to the upstream tree before the option scripts run.
   `0019` (`COLOR_LOAD_FROM`, applies after `0018`): `vglColorLoadFrom(tex)` makes the next scene begun on the bound framebuffer
   load its colour tiles from texture `tex` (same size and format, no MSAA) through the colour surface's background texture, and
   store them into its own buffer as usual: the host's `GXV_EFBSWAP` hands the EFB's colour buffer to a copy texture and continues
   on a fresh one, which then starts with the old picture by the scene's own tile load instead of a full-screen quad
   (`GXV_VGL_LOADFROM=1`, default off; `vglGetColorLoadsFrom()` counts them). UNTESTED on hardware.
   `0020` (`vglEnableAsyncCompiler`, `vglCompileShaderPairToCache`; inert until the host calls the first): a recursive lock around the GLSL
   translator and the runtime compiler (`compile_shader`, `glsl_translator_process`, `glsl_translator_set_process`), and one function a worker thread may
   call to translate a GLSL pair and write what the shader cache lacks, through the same `vgl_cache_compiled_shader` as `glLinkProgram`: no GL object, no
   `sceGxmShaderPatcherRegisterProgram` (the GL thread registers when it later loads the files), none of the GL thread's globals written (the translator
   reads `glsl_override` while the worker holds the lock). The back end's `GXV_ASYNC_SHADERS=1` (default) uses it; a library without the patch leaves every
   shader on the blocking path. Compiled-shader files are byte for byte the same, so the seed's key does not change.
   `0027` (`vglDirectPackedTypes`, applies after `0013`; this entry was labelled 0023 before the patches were renumbered): the direct draw's layout table also takes `VGL_DIRECT_S16` / `VGL_DIRECT_S8`
   (SCE_GXM_ATTRIBUTE_FORMAT_S16 / S8, not normalised), the 16- and 8-bit attributes of the back end's `GXV_VTXPACK` runs (default off).
   Without the patch the back end draws such runs through the ordinary attribute path (`glVertexAttribPointer` with `GL_SHORT` /
   `GL_BYTE`); nothing else changes. Compiled shaders are unchanged (the vertex programs are patched per layout as before).
   `0030` (vp28, direct24; define `VP28_DIRECT_DISPLAY`, on top of vp27's defines, applies on the vp27 tree = vp26 + 0029): display textures. A linear
   A8B8G8R8 texture in 4096-aligned storage (the host makes it with `vglForceAllocAligned` + `vglSetupTexture`) becomes a display texture with
   `vglMakeDisplayTexture(name)`: it gets its own `SceGxmSyncObject`, which every scene of a framebuffer object over that texture begins with (the
   display's release of the buffer is waited for by the GPU), and `vglSetNextFlip(name)` makes the next `vglSwapBuffers` queue that texture to the
   display (its sync object as the entry's new buffer, the previously queued entry's as the old one; vitaGL's own buffers stay where they are)
   instead of vitaGL's back buffer. `vglIsDisplayTexture`, `vglGetDirectFlips/Refusals`, `vglGetDisplayTextures`. A deleted display texture's sync
   object is destroyed 8 frames later. Nothing changes unless the host calls these (agents/direct24/REPORT.txt, GXV_DIRECT_DISPLAY).
   Archive: libvitaGL-reader-vp28.a (the new imports over vp26's: sceGxmSyncObjectDestroy, sceGxmTextureGetWidth/Height/Stride/Type, all in the SDK's stub).
   `0031` (vp29, slow24; define `VP29_SLOW24`, on top of vp28's defines, applies on the vp28 tree = vp27 + 0030): diagnostics for the copy profile and an
   O(1) free-memory figure. `vglMemFree` is `sceClibMspaceMallocStats`, dlmalloc's statistics walk over every chunk of the pool (the SDK also lists a
   `sceClibMspaceMallocStatsFast`), whose headers lie in uncached CDRAM: a question costs in proportion to the blocks in the video pool (~ the live
   textures), and the back end asks on every GXV_EFBSWAP hand-over. `vglMemFreeFast(type)` returns the exact figure taken at its first call (or at
   `vglMemFreeFastResync`) minus the usable bytes of every block `vgl_malloc/calloc/memalign/realloc/free` took and gave back since (drift: a few bytes of
   chunk overhead per block); the back end's `GXV_SLOW24_VRAMFREE=1` uses it. `vglSlowStatsEnable(GL_TRUE)` turns on running counters read by
   `vglGetSlowStats(out, n)` (slots in utils/mem_utils.h: render targets made / destroyed and their time, scene changes with their end and begin parts,
   depth buffers made, allocations / frees / failures per pool and their time, statistics calls, glGenTextures names and slots read, glDeleteTextures,
   glTexImage2D (storage-only), colour attachments and the render targets they gave up, the collector's cycles, list fills, framebuffer objects in use,
   live blocks per pool); the back end turns them on with `GXV_BE_PROF` ("be-prof vitaGL2:" line). `vglMemStatsProbe(type, out[12])` times both
   statistics calls once. Off (not enabled, never asked) nothing changes but one predictable branch per allocation, free, scene change, attachment and
   texture call. With the define off the tree compiles to vp28's code (gxm.o, framebuffers.o, textures.o differ only in `__LINE__` constants).
   Archive: libvitaGL-reader-vp29.a (agents/slow24/work/build-vp29.sh); the one new import over vp28's is `sceClibMspaceMallocStatsFast` (SDK stub
   libSceLibKernel_stub.a).
   `0028` (vp26, scenes23; defines `VP26_SURFACE_RETRY VP26_VALID_REGION VP26_SKIP_COLOR VP26_LOADFROM_INIT`, applies on the vp25 tree):
   `VP26_SURFACE_RETRY`: a scene begun with a colour surface vitaGL changed (0021's cheap load, 0019's load from another buffer) that
   `sceGxmBeginScene` refuses is begun again at once as it is, the first refusal is logged with GXM's error, and no changed surface is
   asked for again (`vglGetColorSurfaceRefusals`); every failed begin is counted (`vglGetSceneBeginFailures`). Before, a refusal went
   to the fail-closed path of 0009: the draw that began the scene was dropped, and three in one frame used up the frame's attempts so
   that the rest of the frame was not submitted (the suspected cause of the console's `GXV_COLOR_LOADSKIP=7` result and of
   `GXV_VGL_LOADFROM`'s black screen; agents/scenes23/REPORT.txt). `vglSetNextSceneValidRegion(xMax, yMax)`: the next scene on the bound
   framebuffer object begun with the GXM valid region (0,0)-(xMax,yMax) (rounded out to 32-pixel tiles; refused: retried without it,
   never asked again; `vglGetValidRegionScenes/Refusals`), for the back end's `GXV_HOIST_GRID` pass (`GXV_VGL_VALIDREGION=1`).
   `vglColorLoadSkipColor(arm, rgba)`: 0021's cheap load with a texture of one colour (bytes 0 / 255, RGBA8 surfaces), for scenes that
   begin with a clear to that colour (`GXV_COLOR_LOADSKIP` bit 16). `vglColorLoadFrom` builds the background descriptor anew with
   `sceGxmTextureInitLinear` (`vglGetColorLoadFromFailures`). Diagnostic: the host's `GXV_GXM_PROBE=1` reads back what each load brings.
   **Patches 0021 to 0032 at a glance** (one line each; the long entries above and the patch headers say more). The `VPnn_*` names are the
   compile-time defines that switch a patch on (a patch without one is always compiled in); everything the back end can ask for is inert
   until the host calls it.

   | Patch | Define | What it does |
   |---|---|---|
   | `0021-color-load-skip` | `COLOR_LOAD_FROM` | `vglColorLoadSkip`: the next scene on the bound framebuffer (or the display) loads its colour tiles from a 4 KiB repeating texture instead of the old picture, for scenes that overwrite every pixel first |
   | `0022-mask-keep-display` | `MASK_KEEP` | `vglSetMaskKeepDisplay`: the display's present scene also keeps the depth/stencil background mask (its own header calls itself patch 0021) |
   | `0023-vp23-f16-uniform-fast` | `VP23_F16_FAST` | float to half conversion of F16 uniform arrays (the `u_tev` upload) with NEON instead of libgcc, bit-identical to `__gnu_f2h_ieee` for all 2^32 inputs |
   | `0024-vp23-depth-stencil-leak-fbb190f` | `VP23_DS_LEAKFIX` | upstream `fbb190f`: a discarded depth buffer that has a stencil surface frees the stencil memory too (Galaxy never uses stencil, so no change for it) |
   | `0025-vp24-native-color-program` | `VP24_NATIVE_COLOR` | `vglProgramNativeColor`: a `__nativecolor` fragment program (programmable blending) is patched without blend info |
   | `0026-vp24-rt-lastframe-on-draw` | `VP24_RT_LASTFRAME` | drawing into a render-target texture counts as a use, so `glDeleteTextures` no longer frees storage the GPU may still write: the Comet Observatory crash fix (with the game's `GXV_RT_DEFER`) |
   | `0027-vgx-direct-packed` | none | the direct draw takes signed 16- and 8-bit attributes (`VGL_DIRECT_S16` / `S8`) for the back end's `GXV_VTXPACK`; `vglDirectPackedTypes()` tells the host the library has it |
   | `0028-vp26-surface-retry-valid-region-skip-color` | `VP26_SURFACE_RETRY` `VP26_VALID_REGION` `VP26_SKIP_COLOR` `VP26_LOADFROM_INIT` | a refused scene begin is retried as it is and counted; a scene can use a GXM valid region; the cheap colour load can use a one-colour texture; `vglColorLoadFrom` builds its descriptor anew |
   | `0029-vp27-display-mode` | `VP27_DISPLAY_MODE` | the display mode (960x544, 720x408, 640x368, 480x272) changes between pictures with no `sceGxmFinish` and no new memory (`vglPrepareDisplayMode`, `vglSetDisplayMode`) |
   | `0030-vp28-direct-display` | `VP28_DIRECT_DISPLAY` | a linear texture becomes a display buffer (`vglMakeDisplayTexture`, `vglSetNextFlip`), so the finished picture is flipped without a copy |
   | `0031-vp29-slow24-stats` | `VP29_SLOW24` | running counters for the copy profile (`vglSlowStatsEnable`, `vglGetSlowStats`) and `vglMemFreeFast`, an O(1) free-memory figure instead of dlmalloc's walk of every chunk |
   | `0032-vp30-display-stride` | none (inside `VP28_DIRECT_DISPLAY`) | the display texture's row length is `ALIGN(width, 8) * 4` for a plain linear texture (`sceGxmTextureGetStride` answers for strided ones only, so 0030 refused every texture); `vglGetDisplayFailure` gives the reason of a refusal |

   **Patches 0033 to 0035 (vp31, test archives only; not in the release archive).** They apply on the vp30 tree (the kit's `libvitaGL-reader-vp30.a` is built
   from it, README of the kit: `agents/gpu23b/build-vgl24.sh`) and are meant for a console A/B. Taken from upstream vitaGL (LGPL-3.0-or-later; the
   patch headers name the commit and author):

   | Patch | Define | What it does |
   |---|---|---|
   | `0033-vp31-vertex-program-mru-compare-6647b18` | none | upstream `6647b18`: the vertex-program cache's MRU compare uses inline 4/8/16-byte compares instead of `sceClibMemcmp`. Same result, same commands |
   | `0034-vp31-uniform-copy-53cc00f` | none | upstream `53cc00f`: `vgl_uniform_copy` inlines the small sizes (the `noinline` helper is gone). Same bytes copied |
   | `0035-vp31-glfinish-no-open-scene` | `VP31_FINISH_NOSCENE` | adapted from upstream `74c5a4a`: `glFinish` ends the open scene and calls `sceGxmFinish` with no scene open (restart through `needs_scene_reset`), instead of ending, beginning a new scene and finishing inside it. Define off = old text. Not tested on hardware |

   Not taken: upstream `9bf3e3f` (glUseProgram early return: already in the base tree, `glUseProgram` of vp30 has it) and `d48d0a0` (builtin memset/memcpy: its hunks
   are in the draw-arrays and draw-elements paths that `DRAW_SPEEDHACK` / the direct draw bypass, and in `ffp.c`, which Galaxy never runs). `-DDISABLE_TILE_CLIPPER`
   (upstream `NO_TILE_CLIPPER=1`) is a build define, not a patch: no sceGxmSetRegionClip for the scissor test; test archive B only.

   **Which archive and defines the 2.3 build uses.** The newest library is **`libvitaGL-reader-vp30.a`** ("vp30": patches up to 0032): the build of
   `nhustdx` plus the options of step 5 and `-DVP23_F16_FAST -DVP23_DS_LEAKFIX -DVP24_NATIVE_COLOR -DVP24_RT_LASTFRAME -DVP26_VALID_REGION
   -DVP26_SKIP_COLOR -DVP26_LOADFROM_INIT -DVP26_SURFACE_RETRY -DVP27_DISPLAY_MODE -DVP28_DIRECT_DISPLAY -DVP29_SLOW24`
   (0032 adds no define; 0021, 0022 and 0027 are covered by the options of step 5 or need none). The Observatory crash fix needs 0026 and its
   define: an archive built without `VP24_RT_LASTFRAME` lacks it, and the back end's display buffer (`GXV_DIRECT_DISPLAY`) needs vp30 (older
   archives log "before vp30"). The archive is an input of the game build; the source of record is the patch series in this folder applied to upstream in order.

3. **[gpu_timers.py](gpu_timers.py)**: `GPU_TIMERS`, the per-scene GPU time the automatic picture
   quality reads (since 5 Nov also the per-scene vertex and fragment completion for the back end's `GXV_GPU_SCENES=1`:
   `vglRtSetupScenes`, `vglRtTagScene`, `vglRtReadScenes`, off until called; host test `tools/test_vgx_gpu_timer_ring.sh`), and `ANISO_SWITCH` (off unless the game asks for it; it acts when a texture's filters are set, in
   `vglSetTexMipFilter`, and may be changed while the game runs: `GXV_AUTOQ_ANISO`).
4. **[fbo_msaa.py](fbo_msaa.py)**: `FBO_MSAA`, multisampling per framebuffer object, for the automatic
   anti-aliasing.
5. **[build-vitagl.sh](build-vitagl.sh)** with the options `DEPTH_HINT UNIFORM_PINGPONG ALIGNED_POOL
   GPU_TIMERS ANISO_SWITCH FBO_MSAA DEPTH_SAMPLE MASK_SKIP MASK_KEEP COLOR_LOAD_FROM` on top of the base set
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

## Faster variant (`nhustdx`)

`FAST=1 tools/vitagl/build-release.sh` (or `build-release.sh nhustdx`) builds the same library with
`DRAW_SPEEDHACK INDICES_SPEEDHACK SAMPLERS_SPEEDHACK GXM_STATE_CACHE HAVE_SCRATCH_MEMORY` added. With the first three alone only `custom_shaders.o` and `ffp.o` differed from
`nhustd` (the exported and imported symbols were the same); the state cache and the scratch option change further objects. `VGL_EXTRA="OPT ..."` adds further options to either
variant for A/B builds. What was checked against how the Galaxy back end calls vitaGL
(`platform/vgx/src/vgx_backend_gl.cpp`):

| Option | For Galaxy |
|---|---|
| `DRAW_SPEEDHACK` | In. Takes each attribute's own stream straight from its buffer. The back end uses buffer objects only (one buffer per draw, every attribute enabled with `glVertexAttribPointer`), draws with `first` 0, so this is the address the strict path already computed; it only skips the packing test and the index scan. |
| `INDICES_SPEEDHACK` | In. Stops vitaGL setting each stream's index source per draw. The back end draws 16-bit indices only (`GL_UNSIGNED_SHORT`), no instancing or divisors, so the source is always the initial 16-bit one. |
| `SAMPLERS_SPEEDHACK` | In. Drops the null test on a program's sampler slots, which is only safe when the compiler numbers a program's samplers 0..n-1 without gaps. In all 687 fragment shaders of the console's compiled-shader cache they are (`u_tex0,u_tex2` get units 0,1); no vertex shader has a sampler. |
| `GXM_STATE_CACHE` | In (a patch of ours, `patches/0011`, not an upstream option). The context keeps the fragment program, vertex program and fragment textures it was given until it is given others, so setting the same ones again changes nothing; vitaGL set them again for every draw (`setup_frag_program`, the texture loop, `sceGxmSetVertexProgram`). Every call of the three setters goes through a wrapper that remembers what the main context holds and drops a call that would set what is already set: a program is the same when it is the same object, a texture when it is the same descriptor address with the same 16 bytes. The memory is forgotten at a scene begin and end, at a precomputed state or draw (vitaGL's own clear, blit and scissor draws), and when a program is unregistered or released (its address may be handed out again). Host test: `tools/test_vitagl_scene_fail.sh` runs one random script of draws (the real state-setting text of the custom shader draw), internal clears and blits, texture changes, program re-creation and scene boundaries with and without the option and compares the state the context would draw with at every draw (for a context that keeps its state across scene begins and for one that forgets it). Not provable on a PC: that the real GXM driver holds state the way the test's model does. |
| Drop `STRICT_DRAW_COMPLIANCE` | Not safe with the Vita build as it is. The non-strict path packs buffer attributes into one stream and writes their absolute buffer offset into sceGxm's 16-bit attribute offset; the Vita build draws with relative indices (a byte offset per draw, `GXV_ABSIDX_DEFAULT=0`), which is above 65535 for most of a frame's vertices. Needs `GXV_ABSIDX=1` as well, and gains little over `DRAW_SPEEDHACK`. |
| `HAVE_VERTEX_LAYOUT_CACHE` | Compiled out unless strict mode is dropped; then it needs the same absolute-index mode, and the offsets change per draw in the relative mode (every draw would miss). |
| `HAVE_SCRATCH_MEMORY` | Compiled in (`build-release.sh`), used only with `GXV_VITA_SCRATCH=1` (default 0: the back end then calls `vglSetupScratchMemory(GL_FALSE, GL_FALSE)` and the library behaves as without the option). What it changes: `glBufferData` / `glBufferSubData` of a `GL_STREAM_*` buffer (and of a `GL_DYNAMIC_*` one, which the back end keeps off) takes its memory from the picture's circular pool (`vgl_reserve_data_pool`) instead of the heap, and `glDeleteBuffers` / a new `glBufferData` leave that memory alone. Nothing else moves: `vglAllocFromScratch` is `gpu_alloc_mapped_temp`, which already took the pool without the option, and the back end does not call it. Why it is safe for Galaxy, by the driver text (host test `tools/test_vitagl_scratch_pool.sh`): the only stream buffers are `g_streamVbo[]` and `g_ibo`, and `UploadStream` gives every one its whole content again at the start of every back-end frame, before that frame's draws (a draw never reads a stream buffer of an earlier frame); static geometry has the `GL_DYNAMIC_DRAW` hint and the dynamic switch is off, so it stays on the heap. The pool of a picture is `swaps mod display buffers`, reset at the swap; an unpresented frame (`vglSubmitWithoutSwap`, which ends in `sceGxmFinish`) keeps taking from the same pool and is complete when it ends. A pool is written again after `buffers` swaps: sceGxm holds at most `displayQueueMaxPendingCount = buffers - 1` display entries, so once swap F has returned picture F-(buffers-1), whose pool is the next one, is complete. That is the promise vitaGL's own per-picture temporary allocations (client arrays, uniform arrays, texture-upload sources) already rest on. Overflow (a picture needing more than 32 MiB / buffers) falls back to a heap block that goes to the collector, which frees it three swaps later. Not provable on a PC: that the real display queue keeps its promise. Gain: two heap allocations and two collector entries (and the later two frees on the collector thread) per back-end frame, and about four pictures of stream data (the plaza measured 83-91 KB a picture) less held in the main-memory pool; the copy of the data stays. Small. |
| `CIRCULAR_POOL_SPEEDHACK` | Not in. One 32 MiB ring replaces the pools, with no reset at a swap, no per-picture partition and no overflow fallback (the pointer wraps to the start). Buffers it holds are fine for Galaxy, but the same ring serves the sources of `sceGxmTransferCopy` texture uploads (a DXT1 level whose size is already block-aligned and at most 2048 high goes the `default:` branch of `gpu_alloc_compressed_texture` with a temporary copy, since `TEXTURE_UPLOADS_SPEEDHACK` is off): a stage load that uploads more than the ring in the time the transfer queue needs wraps onto a source that has not been read, which gives a wrong texture, not a crash. The per-picture pools cost three pointer assignments a swap, so there is nothing to gain. |
| `NO_DEBUG` | The Makefile turns it into `-DSKIP_ERROR_HANDLING`; `-DNO_DEBUG` itself does nothing. That removes the early outs for out-of-handles, out-of-memory, oversize textures and bad arguments (failure paths the back end relies on being survivable). Not exact; negligible gain. |
| `READBACKS_SPEEDHACK` | Removes the scene flush before `glReadPixels` of the framebuffer in use. The player never reads pixels back (captures, verification and probes are desktop or diagnostic), so no gain, and those reads would see unfinished pictures. |
| `MATH_SPEEDHACK` | Only fixed-function `glOrtho`/`glFrustum`; the back end uses none. No effect. |
| `PRIMITIVES_SPEEDHACK` | Not safe. The back end draws `GL_LINES` and `GL_POINTS`; without the restore, every triangle draw after the first line or point draw keeps the line or point polygon mode. |

   `0032` (vp30, direct24 round 2; no new define: inside `VP28_DIRECT_DISPLAY`, applies on the vp29 tree): the display textures' row length. patch 0030 took it from
   `sceGxmTextureGetStride`, which answers for LINEAR_STRIDED textures only (0 for the plain LINEAR ones the back end makes), so every `vglMakeDisplayTexture` was refused
   and the machine switched itself off on the console. The stride is now ALIGN(width, 8) * 4 for a LINEAR texture (vitaGL's own framebuffer rule), `GetStride` only for a
   LINEAR_STRIDED one; the swap's pitch uses the same. New `vglGetDisplayFailure(uint32_t out[10])`: the reason of the last refusal of `vglMakeDisplayTexture` (1-5) or
   `vglSetNextFlip` (6-8) with the texture's name, status, data pointer, format and type words, stride, size and the display textures made; returns the refusal count.

## Licence

vitaGL is GPL-3.0 ([licenses/GPL-3.0.txt](../../licenses/GPL-3.0.txt)); the patches and scripts here
are offered under the same licence.

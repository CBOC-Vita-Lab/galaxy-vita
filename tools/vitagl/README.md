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
3. **[gpu_timers.py](gpu_timers.py)**: `GPU_TIMERS`, the per-scene GPU time the automatic picture
   quality reads, and `ANISO_SWITCH` (off unless the game asks for it; it acts when a texture's filters are set, in
   `vglSetTexMipFilter`, and may be changed while the game runs: `GXV_AUTOQ_ANISO`).
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

## Licence

vitaGL is GPL-3.0 ([licenses/GPL-3.0.txt](../../licenses/GPL-3.0.txt)); the patches and scripts here
are offered under the same licence.

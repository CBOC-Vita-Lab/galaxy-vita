#!/bin/bash
# Builds libvitaGL.a from a prepared vitaGL source folder with a named option set, into
# build/vitagl/<name>/ (lib/, include/). The other archives of the link set (vitaShaRK,
# SceShaccCgExt, math-neon, taiHEN and kubridge stubs) are copied from VGL_LIBS, so the result is a
# complete VGL_DEPS folder for the Galaxy build (tools/vgx_build/build.py).
#
#   VGL_SRC=<prepared vitaGL source> VGL_LIBS=<lib/ + include/ folder> tools/vitagl/build-vitagl.sh <name> "<extra options>"
#
# The 1.0 release uses tools/vitagl/build-release.sh, which prepares the source and calls this script.
#
# The base set (-O3 -ffast-math -mtune=cortex-a9 -mfpu=neon, hard-float ABI, HAVE_SHADER_CACHE
# HAVE_SHARK_LOG LOG_ERRORS STORE_DEPTH_STENCIL SKIP_SPLASHSCREEN STRICT_DRAW_COMPLIANCE) is always
# used. The second argument adds options to it, space separated; a name with "=" is passed as a
# value (for example FOO=2); "-NAME" drops NAME from the base set. Options this script implements as
# source patches of its own copy: ALIGNED_POOL, DEPTH_HINT, UNIFORM_PINGPONG, DEPTH_SAMPLE, MASK_SKIP.
# The prepared source folder itself is never changed.
set -euo pipefail
name=${1:?variant name}
extra=${2:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VGL_SRC=${VGL_SRC:?set VGL_SRC to the prepared vitaGL source folder (tools/vitagl/build-release.sh does)}
VGL_LIBS=${VGL_LIBS:?set VGL_LIBS to the folder with lib/ and include/ of the other Vita libraries}
SDK=${VITASDK:-/opt/vitasdk}
OUT=${OUT:-$ROOT/build/vitagl/$name}
JOBS=${JOBS:-2}
CC=$SDK/bin/arm-vita-eabi-gcc
AR=$SDK/bin/arm-vita-eabi-ar
CXX=$SDK/bin/arm-vita-eabi-g++

# The base options: the port's shader seed key (GXV_VGL_ID in vgx_host_galaxy_vita.cpp) lists them.
base="HAVE_SHADER_CACHE HAVE_SHARK_LOG LOG_ERRORS STORE_DEPTH_STENCIL SKIP_SPLASHSCREEN STRICT_DRAW_COMPLIANCE"
# An option written "-NAME" in the extra list removes NAME from the base set.
keep=""
for d in $base; do
  drop=0
  for x in $extra; do [ "$x" = "-$d" ] && drop=1; done
  [ $drop = 0 ] && keep="$keep $d"
done
defs=""
for d in $keep; do defs="$defs -D$d"; done
for x in $extra; do case "$x" in -*) ;; *) defs="$defs -D$x" ;; esac; done
# GCC 15 needs -Wno-incompatible-pointer-types for the source (its pointer casts are old-style) and
# -mfp16-format=ieee for __fp16 (the existing library records the IEEE 754 half format).
CFLAGS="-O3 -ffast-math -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -mfp16-format=ieee -Wno-incompatible-pointer-types $defs"

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/obj"
cp "$VGL_SRC/vitaGL.h" "$OUT/include/"
cp -r "$VGL_LIBS/include/." "$OUT/include/"
# Every other archive of the link set, from VGL_LIBS.
for a in libvitashark.a libSceShaccCgExt.a libmathneon.a libtaihen_stub.a libkubridge_stub.a; do
  [ -f "$VGL_LIBS/lib/$a" ] && cp "$VGL_LIBS/lib/$a" "$OUT/lib/"
done

# Work on a copy of the source, so the prepared tree is untouched. Two differences from the source
# as it is there, both matching what the Galaxy link expects (its undefined symbols): vgl_log goes to
# msc_vgl_printf, which the galaxy hosts define (the stock header sends it to sceClibPrintf, which
# the galaxy link has no stub for); and C++17 sized deallocation is off (the old library does not
# call operator delete(void*, size_t)).
rm -rf "$OUT/src"
cp -r "$VGL_SRC" "$OUT/src"
sed -i 's|#define vgl_log sceClibPrintf|#ifdef __cplusplus\nextern "C"\n#endif\nint msc_vgl_printf(const char* format, ...);\n#define vgl_log msc_vgl_printf|' "$OUT/src/utils/debug_utils.h"
grep -q "define vgl_log msc_vgl_printf" "$OUT/src/utils/debug_utils.h" || { echo "vgl_log patch did not apply"; exit 1; }
# ALIGNED_POOL (from the extra options): every reservation in vitaGL's circular pool starts on 16 bytes.
# The pool hands out exactly the size asked for, so an odd-sized U16 index upload used to leave the next
# float data unaligned (the Strikers Vita port hit an ARM data abort from it). Only the copy is changed.
if echo " $extra " | grep -q ' ALIGNED_POOL '; then
  sed -i 's|^uint8_t \*vgl_reserve_data_pool(uint32_t size) {|&\n\tsize = (size + 15u) \& ~15u; /* ALIGNED_POOL */|' "$OUT/src/vgl.c"
  grep -q 'ALIGNED_POOL \*/' "$OUT/src/vgl.c" || { echo "ALIGNED_POOL patch did not apply"; exit 1; }
fi
# DEPTH_HINT (from the extra options): vglSetNextSceneDepthModes(load, store) tells vitaGL that the next scene begun on the
# framebuffer object that is bound NOW does not need its depth/stencil surface loaded and/or stored. With STORE_DEPTH_STENCIL
# vitaGL forces both on every scene of every framebuffer object (gxm.c init_depth_stencil_buffer), which is a full-size read and
# write of a depth buffer even for a copy target that never tests depth. The hint applies to one scene only: every scene begin
# sets both modes again (to the hint, or to enabled), so a surface never keeps a mode from an earlier scene. A hint made for
# another framebuffer object than the one the scene is on is dropped. Only the copy is changed.
if echo " $extra " | grep -q ' DEPTH_HINT '; then
  python3 - "$OUT/src" <<'PYEOF'
import sys
d = sys.argv[1]
def sub(path, old, new, count=1):
    s = open(path).read()
    assert s.count(old) == count, (path, old, s.count(old))
    open(path, "w").write(s.replace(old, new))
g = d + "/gxm.c"
sub(g, "static GLboolean scene_not_flushed = GL_FALSE; // Flag for when a scene had been forcibly flushed as last operation\n",
"""static GLboolean scene_not_flushed = GL_FALSE; // Flag for when a scene had been forcibly flushed as last operation
#ifdef DEPTH_HINT
static GLboolean depth_hint_valid = GL_FALSE;
static GLboolean depth_hint_load = GL_TRUE, depth_hint_store = GL_TRUE;
static framebuffer *depth_hint_fb = NULL;
void vglSetNextSceneDepthModes(GLboolean load, GLboolean store) {
	depth_hint_valid = GL_TRUE;
	depth_hint_load = load;
	depth_hint_store = store;
	depth_hint_fb = active_write_fb;
}
// Called right before sceGxmBeginScene with the scene's depth/stencil surface.
static void apply_depth_hint(SceGxmDepthStencilSurface *ds, framebuffer *fb) {
	GLboolean load = GL_TRUE, store = GL_TRUE;
	if (depth_hint_valid && depth_hint_fb == fb) {
		load = depth_hint_load;
		store = depth_hint_store;
	}
	depth_hint_valid = GL_FALSE;
	if (!ds)
		return;
	sceGxmDepthStencilSurfaceSetForceLoadMode(ds, load ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(ds, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
}
#endif
""")
sub(g, """			int r = sceGxmBeginScene(gxm_context, 0, gxm_render_target,
				NULL, NULL,
				gxm_sync_objects[gxm_back_buffer_index],
				&gxm_color_surfaces[gxm_back_buffer_index],
				&gxm_depth_stencil_surface);
#ifdef LOG_ERRORS""", """#ifdef DEPTH_HINT
			apply_depth_hint(&gxm_depth_stencil_surface, NULL);
#endif
			int r = sceGxmBeginScene(gxm_context, 0, gxm_render_target,
				NULL, NULL,
				gxm_sync_objects[gxm_back_buffer_index],
				&gxm_color_surfaces[gxm_back_buffer_index],
				&gxm_depth_stencil_surface);
#ifdef LOG_ERRORS""")
sub(g, """#ifdef HAVE_SHARED_RENDERTARGETS
			render_target *fbo_rt = (render_target *)active_write_fb->target;
#ifdef RECYCLE_RENDERTARGETS
			fbo_rt->last_frame = vgl_framecount;
#endif
			int r = sceGxmBeginScene(""", """#ifdef DEPTH_HINT
			apply_depth_hint(active_write_fb->depthbuffer_ptr, active_write_fb);
#endif
#ifdef HAVE_SHARED_RENDERTARGETS
			render_target *fbo_rt = (render_target *)active_write_fb->target;
#ifdef RECYCLE_RENDERTARGETS
			fbo_rt->last_frame = vgl_framecount;
#endif
			int r = sceGxmBeginScene(""")
sub(d + "/vitaGL.h", "void vglUseTripleBuffering(GLboolean usage);\n", "void vglUseTripleBuffering(GLboolean usage);\nvoid vglSetNextSceneDepthModes(GLboolean load, GLboolean store); /* DEPTH_HINT builds */\n")
print("DEPTH_HINT patch applied")
PYEOF
  [ $? = 0 ] || { echo "DEPTH_HINT patch did not apply"; exit 1; }
fi
# UNIFORM_PINGPONG (from the extra options): the uniform arena rollover without the GPU drain. Stock behaviour: when the 8 MiB
# arena of uniform data is full (every 56 pictures in the castle plaza, about every 1.8 s) vitaGL allocates a fresh 8 MiB block, ENDS
# the open scene, waits in sceGxmFinish for the whole GPU queue, copies the cached default uniforms over, begins the scene again
# (a second scene for the same picture: its colour and depth are loaded and stored once more) and frees the old block. On the
# console that is a 65-70 ms picture instead of 31 ms at every rollover (the slow back-end frames of the 17 Oct battery coincide
# with the rollover log lines to the millisecond). With this option two 8 MiB arenas are kept and used in turn; the arena being
# entered again is only written when the GPU has reached the notification value recorded when it was left (the scene that was
# open then had value+1, and the fence is the same one the occlusion queries use), so the rollover is a pointer switch and a copy
# of the cached defaults, without ending the scene. If the GPU has not got that far yet (a burst of uniform data), it does what the
# stock code does (end the scene, sceGxmFinish), so nothing is ever overwritten while in use. Only the copy is changed.
if echo " $extra " | grep -q ' UNIFORM_PINGPONG '; then
  python3 - "$OUT/src" <<'PYEOF'
import sys
d = sys.argv[1]
p = d + "/utils/gxm_utils.c"
s = open(p).read()
old_vars = "static uint32_t unif_rollovers = 0;\n"
assert s.count(old_vars) == 1
s = s.replace(old_vars, old_vars + """#ifdef UNIFORM_PINGPONG
static uint8_t *unif_spare = NULL;       // the other arena, kept allocated
static uint32_t unif_spare_fence = 0;    // the spare may be written once the GPU's notification value has reached this
static uint32_t unif_pp_fast = 0, unif_pp_waits = 0;
#endif
""")
old_setup = """		unif_pool = vglAllocUniformPool();
		vgl_log("[VGL_UNIFORM_LIFETIME] active=scene-retire-cache-migrate-v1 arena_bytes=%u peak_arenas=2\\n", UNIFORM_CIRCULAR_POOL_SIZE);"""
assert s.count(old_setup) == 1
s = s.replace(old_setup, """		unif_pool = vglAllocUniformPool();
#ifdef UNIFORM_PINGPONG
		unif_spare = vglAllocUniformPool();
		unif_spare_fence = 0;
		vgl_log("[VGL_UNIFORM_LIFETIME] active=pingpong-v1 arena_bytes=%u arenas=2 (the scene is not ended at a rollover unless the GPU is behind)\\n", UNIFORM_CIRCULAR_POOL_SIZE);
#else
		vgl_log("[VGL_UNIFORM_LIFETIME] active=scene-retire-cache-migrate-v1 arena_bytes=%u peak_arenas=2\\n", UNIFORM_CIRCULAR_POOL_SIZE);
#endif""")
a = s.index("static __attribute__((noinline, cold)) void vglRolloverUniformPool(")
b = s.index("void *vglReserveUniformCircularPoolBuffer(")
old_fn = s[a:b]
new_fn = """#ifdef UNIFORM_PINGPONG
static __attribute__((noinline, cold)) void vglRolloverUniformPool(uint32_t size, uint32_t reserved_size) {
	uint32_t vert_size = vgl_def_vert_buf ? VGL_ALIGN(vgl_def_vert_buf_size, 16) : 0;
	uint32_t frag_size = vgl_def_frag_buf ? VGL_ALIGN(vgl_def_frag_buf_size, 16) : 0;
	if (vert_size > UNIFORM_CIRCULAR_POOL_SIZE - reserved_size ||
		frag_size > UNIFORM_CIRCULAR_POOL_SIZE - reserved_size - vert_size) {
		vgl_log("[VGL_UNIFORM_LIFETIME] fatal: cache/request exceed backing capacity\\n");
		abort();
	}
	GLboolean resume = GL_FALSE;
	uint32_t leaving_fence;
	if ((int32_t)(*(volatile uint32_t *)query_fence.address - unif_spare_fence) < 0) {
		// The GPU is still working on scenes that use the spare arena: wait for everything, as the stock code does.
		resume = vglFinishUniformPoolScene();
		unif_pp_waits++;
		leaving_fence = query_fence.value;
	} else {
		unif_pp_fast++;
		leaving_fence = query_fence.value + 1;  // (the scene open now, or the next one, ends with this value)
	}
	uint8_t *next_pool = unif_spare;
	uint32_t next_idx = 0;
	if (vgl_def_vert_buf) {
		vgl_fast_memcpy(next_pool, vgl_def_vert_buf, vgl_def_vert_buf_size);
		vgl_def_vert_buf = next_pool;
		next_idx = vert_size;
	}
	if (vgl_def_frag_buf) {
		vgl_fast_memcpy(next_pool + next_idx, vgl_def_frag_buf, vgl_def_frag_buf_size);
		vgl_def_frag_buf = next_pool + next_idx;
		next_idx += frag_size;
	}
	uint8_t *old_pool = unif_pool;
	uint32_t old_idx = unif_idx;
	unif_pool = next_pool;
	unif_idx = next_idx;
	unif_spare = old_pool;
	unif_spare_fence = leaving_fence;
	vglResumeUniformPoolScene(resume);
	vglRestoreVertexUniformBuffer();
	vglRestoreFragmentUniformBuffer();
	unif_rollovers++;
	if (unif_rollovers <= 8 || unif_rollovers % 600 == 0) {
		vgl_log("[VGL_UNIFORM_LIFETIME] rollover=%u frame=%u old_bytes=%u cached_bytes=%u request=%u resumed=%u pingpong fast=%u waits=%u\\n",
			unif_rollovers, vgl_framecount, old_idx, next_idx, size, resume, unif_pp_fast, unif_pp_waits);
	}
}
#else
""" + old_fn + """#endif

"""
s = s[:a] + new_fn + s[b:]
open(p, "w").write(s)
print("UNIFORM_PINGPONG patch applied")
PYEOF
  [ $? = 0 ] || { echo "UNIFORM_PINGPONG patch did not apply"; exit 1; }
fi
# DEPTH_SAMPLE (from the extra options): GLint vglFramebufferDepthTexture(fb) makes the texture bound on the
# active unit (GL_TEXTURE_2D) show the depth buffer of framebuffer object `fb` as it is in memory: what its last scene stored. vitaGL's
# depth surfaces are SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, DF32M, rows of VGL_ALIGN(width, 32) samples (gxm.c,
# init_depth_stencil_buffer_ms), so the texture is DF32M, that row wide and `height` tall, point sampled, clamped; it returns the row
# width (a caller maps u to [0, width / row]) or 0: no depth buffer ready (before the object's first scene, or let go at a change of
# size), multisampled (the global mode, or with FBO_MSAA the object's own: 2 or 4 samples a pixel), or a texture with storage of its
# own (never repointed: the texture only points at the surface and owns no memory, so deleting it frees nothing). Call it again before
# each use (the surface moves when the object changes size). Unlike vglTexImageDepthBuffer it names the object and changes no
# load/store mode (the scenes' hints decide; STORE_DEPTH_STENCIL stores by default). The back end samples the EFB's depth this way to
# give a reduced-size target the scene's depth (GXV_FXHALF). Same layout as the Strikers port's vglStrikersBindFramebufferDepth
# (160c3f9), which this follows for the row width, the ready check and the storage check. Only the copy is changed.
if echo " $extra " | grep -q ' DEPTH_SAMPLE '; then
  python3 - "$OUT/src" <<'PYEOF'
import sys
d = sys.argv[1]
def sub(path, old, new, count=1):
    s = open(path).read()
    assert s.count(old) == count, (path, old, s.count(old))
    open(path, "w").write(s.replace(old, new))
sub(d + "/framebuffers.c", "/* vgl* */\n", """/* vgl* */

#ifdef DEPTH_SAMPLE
GLint vglFramebufferDepthTexture(GLuint fb) {
	THREAD_SAFE()
	framebuffer *f = (framebuffer *)fb;
	if (!f || !f->active || !f->depthbuffer_ptr || !(f->depthbuffer_state & DEPTHBUFFER_READY) || !f->depthbuffer_ptr->depthData)
		return 0;
	if (msaa_mode != SCE_GXM_MULTISAMPLE_NONE)
		return 0;
#ifdef FB_MSAA
	if (FB_MSAA(f) != SCE_GXM_MULTISAMPLE_NONE) /* FBO_MSAA builds: this object's own mode */
		return 0;
#endif
	texture_unit *tex_unit = &texture_units[server_texture_unit];
	texture *tex = &texture_slots[tex_unit->tex_id[0]];
	if (tex->data || tex->palette_data)
		return 0;
	const int row = VGL_ALIGN(f->width, SCE_GXM_TILE_SIZEX);
	if (sceGxmTextureInitLinear(&tex->gxm_tex, f->depthbuffer_ptr->depthData, SCE_GXM_TEXTURE_FORMAT_DF32M, row, f->height, 0) < 0)
		return 0;
	tex->min_filter = tex->mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
	tex->u_mode = tex->v_mode = SCE_GXM_TEXTURE_ADDR_CLAMP;
	tex->use_mips = GL_FALSE;
	vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
	vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
	vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
	vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
	vglSetTexMipmapCount(&tex->gxm_tex, 0);
	tex->status = TEX_VALID;
	return row;
}
#endif
""")
sub(d + "/vitaGL.h", "void vglUseTripleBuffering(GLboolean usage);\n", "void vglUseTripleBuffering(GLboolean usage);\nGLint vglFramebufferDepthTexture(GLuint fb); /* DEPTH_SAMPLE builds: the row width in samples, 0 when refused */\n")
print("DEPTH_SAMPLE patch applied")
PYEOF
  [ $? = 0 ] || { echo "DEPTH_SAMPLE patch did not apply"; exit 1; }
fi
# MASK_SKIP (from the extra options, needs DEPTH_HINT; after the Strikers port's rt.c): a scene that does not load its
# depth/stencil surface starts with the surface's background mask (0 everywhere), so the full-target quad that clears the scissor mask
# before the region quad (tests.c update_scissor_test, drawn at the start of every framebuffer-object scene under STORE_DEPTH_STENCIL when
# the scissor test is on) is left out for it, as upstream vitaGL does at every scene start without STORE_DEPTH_STENCIL. Only at the start
# of such a scene (the flag is dropped at the end of the scene_reset that set it). vglGetMaskSkips() counts the quads left out.
if echo " $extra " | grep -q ' MASK_SKIP '; then
  echo " $extra " | grep -q ' DEPTH_HINT ' || { echo "MASK_SKIP needs DEPTH_HINT"; exit 1; }
  python3 - "$OUT/src" <<'PYEOF'
import sys
d = sys.argv[1]
def sub(path, old, new, count=1):
    s = open(path).read()
    assert s.count(old) == count, (path, old, s.count(old))
    open(path, "w").write(s.replace(old, new))
g = d + "/gxm.c"
sub(g, """	depth_hint_valid = GL_FALSE;
	if (!ds)
		return;
""", """	depth_hint_valid = GL_FALSE;
#ifdef MASK_SKIP
	vgl_mask_skip_next = ds && !load;
#endif
	if (!ds)
		return;
""")
sub(g, "#ifdef DEPTH_HINT\nstatic GLboolean depth_hint_valid = GL_FALSE;\n", """#ifdef MASK_SKIP
GLboolean vgl_mask_skip_next = GL_FALSE; // tests.c update_scissor_test: the scene began without loading its mask
uint32_t vgl_mask_skips = 0;
uint32_t vglGetMaskSkips(void) { return vgl_mask_skips; }
#endif
#ifdef DEPTH_HINT
static GLboolean depth_hint_valid = GL_FALSE;
""")
sub(g, """	if (dirty_scissor_state) {
		update_scissor_test();
	}
}
""", """	if (dirty_scissor_state) {
		update_scissor_test();
	}
#ifdef MASK_SKIP
	vgl_mask_skip_next = GL_FALSE; // (only for the update at the start of the scene that set it)
#endif
}
""")
t = d + "/tests.c"
sub(t, """#ifdef STORE_DEPTH_STENCIL
	if (scissor_test_state) {
#else""", """#ifdef STORE_DEPTH_STENCIL
#ifdef MASK_SKIP
	extern GLboolean vgl_mask_skip_next;
	extern uint32_t vgl_mask_skips;
	if (vgl_mask_skip_next) {
		vgl_mask_skip_next = GL_FALSE;
		if (scissor_test_state) {
			vgl_mask_skips++;
			vector4f_convert_to_local_space(scissor_test_vertices, region.x, region.y, region.w, region.h);
		}
	} else
#endif
	if (scissor_test_state) {
#else""")
sub(d + "/vitaGL.h", "void vglUseTripleBuffering(GLboolean usage);\n", "void vglUseTripleBuffering(GLboolean usage);\nuint32_t vglGetMaskSkips(void); /* MASK_SKIP builds */\n")
print("MASK_SKIP patch applied")
PYEOF
  [ $? = 0 ] || { echo "MASK_SKIP patch did not apply"; exit 1; }
fi
cd "$OUT/src"
srcs=$(find . \( -name '*.c' -o -name '*.cpp' \) | sed 's|^\./||' | sort)
echo "vitaGL $name: $(echo "$srcs" | wc -l) sources, options:$defs"
n=0
for s in $srcs; do
  o="$OUT/obj/$(echo "$s" | tr '/' '_' | sed 's/\.cpp$/.o/; s/\.c$/.o/')"
  case "$s" in
    *.cpp) echo "$CXX $CFLAGS -std=gnu++17 -fno-sized-deallocation -I. -Iutils -I$VGL_LIBS/include -c $s -o $o" ;;
    *)     echo "$CC $CFLAGS -I. -Iutils -I$VGL_LIBS/include -c $s -o $o" ;;
  esac
done > "$OUT/compile.txt"
# Compile JOBS at a time.
cat "$OUT/compile.txt" | xargs -P "$JOBS" -I{} sh -c '{} 2>>'"$OUT"'/errors.txt' || { echo "compile failed: see $OUT/errors.txt"; exit 1; }
"$AR" rcs "$OUT/lib/libvitaGL.a" "$OUT"/obj/*.o
echo "built $OUT/lib/libvitaGL.a ($(stat -c %s "$OUT/lib/libvitaGL.a") bytes, $(ls "$OUT"/obj/*.o | wc -l) objects)"

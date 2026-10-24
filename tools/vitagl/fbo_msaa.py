#!/usr/bin/env python3
"""vitaGL per-framebuffer multisampling (option FBO_MSAA), for the back end's automatic anti-aliasing.

Stock vitaGL has one multisample mode for everything (vglInit's msaa argument, the global msaa_mode): the display, every
framebuffer object's render target, colour surface and hidden depth buffer, and every patched fragment program. The
galaxy back end wants it on the EFB only (the copies, passes and atlases gain nothing from it and would pay for it), and
switched at run time between pictures. With FBO_MSAA:

  GLboolean vglFramebufferMultisample(GLuint framebuffer, GLint samples)   (1, 2 or 4; vitaGL.h)
      sets the framebuffer object's own mode. Its render target and hidden depth buffer are let go (the garbage collector
      frees them once the GPU is done with them) and made again at the next scene in the new mode (the depth buffer with
      2 or 4 samples a pixel, the 4x one twice as wide in samples, as vitaGL does for the display); the colour surface is
      set up again on the same texture, with SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE when multisampled: the samples
      are averaged into the texture when the scene stores it, so the texture (what copies sample, what glReadPixels and
      the EFB swap hand over) only ever holds the resolved picture. What the texture held stays: the next scene loads it.
      If the object is the one in use, the open scene is ended at the next draw. A new framebuffer object starts with
      the global mode (what it had before this option).
  cur_msaa (gxm.c) follows the framebuffer in use (scene_reset); every fragment program is patched for it: the shader
      programs' patched-program cache (custom_shaders.c) and the fixed-function one (ffp.c) also re-patch when it
      changes (sceGxmShaderPatcher finds an identical program it already made), and the clear and blit programs get one
      patched copy per mode, made on first use.

The display keeps the global mode. Not for HAVE_SHARED_RENDERTARGETS builds (render targets shared by size).
Usage: fbo_msaa.py <dir that holds the vitaGL source folder>   (the copy is changed in place; build with -DFBO_MSAA)
"""
import sys

d = sys.argv[1] + '/' + 'sour' + 'ce'


def sub(path, old, new, count=1):
    p = d + '/' + path
    s = open(p).read()
    assert s.count(old) == count, (path, old, s.count(old))
    open(p, 'w').write(s.replace(old, new))


# ---- shared.h: the field, cur_msaa, FB_MSAA, the patch macro, the colour surface of an attached texture
sub('shared.h', '''	texture *tex;
} framebuffer;''', '''	texture *tex;
#ifdef FBO_MSAA
	SceGxmMultisampleMode msaa; // this framebuffer's multisampling (vglFramebufferMultisample)
#endif
} framebuffer;''')
sub('shared.h', 'extern SceGxmMultisampleMode msaa_mode;\n', '''extern SceGxmMultisampleMode msaa_mode;
#ifdef FBO_MSAA
#ifdef HAVE_SHARED_RENDERTARGETS
#error "FBO_MSAA: render targets shared by size would mix multisample modes"
#endif
extern SceGxmMultisampleMode cur_msaa; // the multisample mode of the framebuffer in use (scene_reset)
#define FB_MSAA(fb) ((fb)->msaa)
SceGxmFragmentProgram *vgl_msaa_fp(int blit, GLboolean is_float); // clear (0) / blit (1) program for cur_msaa
#else
#define FB_MSAA(fb) msaa_mode
#endif
''')
sub('shared.h', '#define rebuild_frag_shader(x, y, z, w) patch_fragment_program(x, w, msaa_mode, &blend_info.info, z, y) // Creates a new patched fragment program with proper blend settings',
    '''#ifdef FBO_MSAA
#define rebuild_frag_shader(x, y, z, w) patch_fragment_program(x, w, cur_msaa, &blend_info.info, z, y) // Creates a new patched fragment program with proper blend settings (for the framebuffer in use)
#else
#define rebuild_frag_shader(x, y, z, w) patch_fragment_program(x, w, msaa_mode, &blend_info.info, z, y) // Creates a new patched fragment program with proper blend settings
#endif''')
sub('shared.h', '''		sceGxmColorSurfaceInit(&fb->colorbuffer, get_color_from_texture(fmt), SCE_GXM_COLOR_SURFACE_LINEAR,
			msaa_mode == SCE_GXM_MULTISAMPLE_NONE ? SCE_GXM_COLOR_SURFACE_SCALE_NONE : SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE,''',
    '''		sceGxmColorSurfaceInit(&fb->colorbuffer, get_color_from_texture(fmt), SCE_GXM_COLOR_SURFACE_LINEAR,
			FB_MSAA(fb) == SCE_GXM_MULTISAMPLE_NONE ? SCE_GXM_COLOR_SURFACE_SCALE_NONE : SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE,''')
sub('shared.h', 'void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil); // Creates depth and stencil surfaces',
    '''void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil); // Creates depth and stencil surfaces
void init_depth_stencil_buffer_ms(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil, SceGxmMultisampleMode mode); // ... with a given multisample mode''')

# ---- gxm.c: cur_msaa, render targets and depth buffers of a framebuffer's own mode
sub('gxm.c', 'GLboolean is_fbo_float = GL_FALSE; // Current framebuffer mode\n', '''GLboolean is_fbo_float = GL_FALSE; // Current framebuffer mode
#ifdef FBO_MSAA
SceGxmMultisampleMode cur_msaa = SCE_GXM_MULTISAMPLE_NONE; // Current framebuffer's multisample mode
#endif
''')
sub('gxm.c', '''static inline __attribute__((always_inline)) int setup_render_target(SceGxmRenderTarget **rt, int w, int h, int refs) {
	SceGxmRenderTargetParams renderTargetParams;''', '''static inline __attribute__((always_inline)) int setup_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
	SceGxmRenderTargetParams renderTargetParams;''')
sub('gxm.c', '''	renderTargetParams.multisampleMode = msaa_mode;
	renderTargetParams.driverMemBlock = -1;
	return sceGxmCreateRenderTarget(&renderTargetParams, rt);
}
''', '''	renderTargetParams.multisampleMode = mode;
	renderTargetParams.driverMemBlock = -1;
	return sceGxmCreateRenderTarget(&renderTargetParams, rt);
}
static inline __attribute__((always_inline)) int setup_render_target(SceGxmRenderTarget **rt, int w, int h, int refs) {
	return setup_render_target_ms(rt, w, h, refs, msaa_mode);
}
''')
sub('gxm.c', 'void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil) {\n',
    'void init_depth_stencil_buffer_ms(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil, SceGxmMultisampleMode mode) {\n')
sub('gxm.c', '	if (msaa_mode == SCE_GXM_MULTISAMPLE_2X)\n		depth_stencil_samples *= 2;\n	else if (msaa_mode == SCE_GXM_MULTISAMPLE_4X)\n',
    '	if (mode == SCE_GXM_MULTISAMPLE_2X)\n		depth_stencil_samples *= 2;\n	else if (mode == SCE_GXM_MULTISAMPLE_4X)\n')
sub('gxm.c', '		msaa_mode == SCE_GXM_MULTISAMPLE_4X ? depth_stencil_width * 2 : depth_stencil_width,\n',
    '		mode == SCE_GXM_MULTISAMPLE_4X ? depth_stencil_width * 2 : depth_stencil_width,\n')
sub('gxm.c', 'void init_display_depth_stencil_surfaces(void) {\n', '''void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil) {
	init_depth_stencil_buffer_ms(w, h, surface, has_stencil, msaa_mode);
}

void init_display_depth_stencil_surfaces(void) {
''')
sub('gxm.c', '		is_fbo_float = in_use_framebuffer ? in_use_framebuffer->is_float : GL_FALSE;\n', '''		is_fbo_float = in_use_framebuffer ? in_use_framebuffer->is_float : GL_FALSE;
#ifdef FBO_MSAA
		cur_msaa = in_use_framebuffer ? in_use_framebuffer->msaa : msaa_mode;
#endif
''')
sub('gxm.c', '				init_depth_stencil_buffer(active_write_fb->width, active_write_fb->height, &active_write_fb->depthbuffer, active_write_fb->depthbuffer_state == DEPTHBUFFER_WANTS_STENCIL);\n',
    '				init_depth_stencil_buffer_ms(active_write_fb->width, active_write_fb->height, &active_write_fb->depthbuffer, active_write_fb->depthbuffer_state == DEPTHBUFFER_WANTS_STENCIL, FB_MSAA(active_write_fb));\n')
sub('gxm.c', '				int r = setup_render_target(&active_write_fb->target, active_write_fb->width, active_write_fb->height, gxm_fbo_rt_size);\n',
    '				int r = setup_render_target_ms(&active_write_fb->target, active_write_fb->width, active_write_fb->height, gxm_fbo_rt_size, FB_MSAA(active_write_fb));\n')

# ---- custom_shaders.c: the patched-program cache keyed on the mode too
sub('custom_shaders.c', '	GLboolean is_fbo_float;\n	uint8_t num_glsl_attr;\n', '''	GLboolean is_fbo_float;
#ifdef FBO_MSAA
	SceGxmMultisampleMode msaa; // the mode fprog was patched for
#endif
	uint8_t num_glsl_attr;
''')
sub('custom_shaders.c', '''#define setup_frag_program() \\
	if ((p->blend_info.raw != blend_info.raw) || (is_fbo_float != p->is_fbo_float)) { \\
		p->is_fbo_float = is_fbo_float; \\''', '''#ifdef FBO_MSAA
#define FRAG_MSAA_DIRTY (p->msaa != cur_msaa)
#define FRAG_MSAA_SET p->msaa = cur_msaa;
#else
#define FRAG_MSAA_DIRTY 0
#define FRAG_MSAA_SET
#endif
#define setup_frag_program() \\
	if ((p->blend_info.raw != blend_info.raw) || (is_fbo_float != p->is_fbo_float) || FRAG_MSAA_DIRTY) { \\
		p->is_fbo_float = is_fbo_float; \\
		FRAG_MSAA_SET \\''')
sub('custom_shaders.c', '''		rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
		p->is_fbo_float = is_fbo_float;
''', '''		rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
		p->is_fbo_float = is_fbo_float;
#ifdef FBO_MSAA
		p->msaa = cur_msaa;
#endif
''')

# ---- ffp.c: the fixed-function program re-patched when the mode changes
sub('ffp.c', 'SceGxmFragmentProgram *ffp_fragment_program_patched; // Patched fragment program for the fixed function pipeline implementation\n',
    '''SceGxmFragmentProgram *ffp_fragment_program_patched; // Patched fragment program for the fixed function pipeline implementation
#ifdef FBO_MSAA
static SceGxmMultisampleMode ffp_msaa_patched = SCE_GXM_MULTISAMPLE_NONE; // the mode it was patched for
#endif
''')
sub('ffp.c', '	GLboolean ffp_dirty_frag_blend = ffp_blend_info.raw != blend_info.raw;\n', '''	GLboolean ffp_dirty_frag_blend = ffp_blend_info.raw != blend_info.raw;
#ifdef FBO_MSAA
	if (ffp_msaa_patched != cur_msaa)
		ffp_dirty_frag_blend = GL_TRUE;
#endif
''')
sub('ffp.c', '''		// Updating current fixed function pipeline blend config
		ffp_blend_info.raw = blend_info.raw;
''', '''		// Updating current fixed function pipeline blend config
		ffp_blend_info.raw = blend_info.raw;
#ifdef FBO_MSAA
		ffp_msaa_patched = cur_msaa;
#endif
''')

# ---- clear and blit programs for the mode in use
sub('misc.c', '	sceGxmSetFragmentProgram(gxm_context, is_fbo_float ? clear_fragment_program_float_patched : clear_fragment_program_patched);\n', '''#ifdef FBO_MSAA
	sceGxmSetFragmentProgram(gxm_context, vgl_msaa_fp(0, is_fbo_float));
#else
	sceGxmSetFragmentProgram(gxm_context, is_fbo_float ? clear_fragment_program_float_patched : clear_fragment_program_patched);
#endif
''')
sub('framebuffers.c', '''	if (is_fbo_float) {
		sceGxmSetFragmentProgram(gxm_context, blit_fragment_program_float_patched);
	} else {
		sceGxmSetFragmentProgram(gxm_context, blit_fragment_program_patched);
	}
''', '''#ifdef FBO_MSAA
	sceGxmSetFragmentProgram(gxm_context, vgl_msaa_fp(1, is_fbo_float));
#else
	if (is_fbo_float) {
		sceGxmSetFragmentProgram(gxm_context, blit_fragment_program_float_patched);
	} else {
		sceGxmSetFragmentProgram(gxm_context, blit_fragment_program_patched);
	}
#endif
''')
sub('vgl.c', 'SceGxmMultisampleMode msaa_mode = SCE_GXM_MULTISAMPLE_NONE;\n', '''SceGxmMultisampleMode msaa_mode = SCE_GXM_MULTISAMPLE_NONE;
#ifdef FBO_MSAA
// The clear (0) and blit (1) fragment programs patched for the framebuffer in use: the ones made at start-up for the
// global mode, otherwise one per mode and output format, made on first use.
SceGxmFragmentProgram *vgl_msaa_fp(int blit, GLboolean is_float) {
	if (cur_msaa == msaa_mode)
		return blit ? (is_float ? blit_fragment_program_float_patched : blit_fragment_program_patched) : (is_float ? clear_fragment_program_float_patched : clear_fragment_program_patched);
	static SceGxmFragmentProgram *made[2][2][3];
	const int m = cur_msaa == SCE_GXM_MULTISAMPLE_4X ? 2 : (cur_msaa == SCE_GXM_MULTISAMPLE_2X ? 1 : 0);
	SceGxmFragmentProgram **p = &made[blit ? 1 : 0][is_float ? 1 : 0][m];
	if (!*p)
		patch_fragment_program(blit ? blit_fragment_id : clear_fragment_id, is_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, cur_msaa, NULL, NULL, p);
	return *p;
}
#endif
''')

# ---- framebuffers.c: new objects start with the global mode; vglFramebufferMultisample
sub('framebuffers.c', '			framebuffers[i].tex = NULL;\n', '''			framebuffers[i].tex = NULL;
#ifdef FBO_MSAA
			framebuffers[i].msaa = msaa_mode;
#endif
''')
p = d + '/framebuffers.c'
s = open(p).read()
s += '''
#ifdef FBO_MSAA
GLboolean vglFramebufferMultisample(GLuint id, GLint samples) {
	THREAD_SAFE()

	framebuffer *fb = (framebuffer *)id;
	if (!fb || !fb->active)
		return GL_FALSE;
	const SceGxmMultisampleMode m = samples >= 4 ? SCE_GXM_MULTISAMPLE_4X : (samples >= 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE);
	if (fb->msaa == m)
		return GL_TRUE;
	fb->msaa = m;
	// The render target and the hidden depth buffer of the old mode go to the garbage collector (freed once the GPU is
	// past them); the next scene makes both again (gxm.c scene_reset).
	if (fb->target) {
		mark_rt_as_dirty(fb->target);
		fb->target = NULL;
	}
	if (fb->depthbuffer_ptr) {
#ifndef DEPTH_STENCIL_HACK
		mark_as_dirty(fb->depthbuffer_ptr->depthData);
		if (fb->depthbuffer_ptr->stencilData)
			mark_as_dirty(fb->depthbuffer_ptr->stencilData);
#endif
		fb->depthbuffer_ptr = NULL;
	}
	fb->depthbuffer_state &= ~DEPTHBUFFER_READY;
	// The colour surface on the same texture, set up for the new mode (downscaled into the texture when multisampled).
	if (fb->tex)
		_glFramebufferTexture2D(fb, GL_COLOR_ATTACHMENT0, fb->tex, 1);
	if (fb == in_use_framebuffer)
		dirty_framebuffer = GL_TRUE;
	return GL_TRUE;
}
#endif
'''
open(p, 'w').write(s)
sub('vitaGL.h', 'GLboolean vglInitWithCustomThreshold(int pool_size, int width, int height, int ram_threshold, int cdram_threshold, int phycont_threshold, int cdlg_threshold, SceGxmMultisampleMode msaa);\n',
    '''GLboolean vglInitWithCustomThreshold(int pool_size, int width, int height, int ram_threshold, int cdram_threshold, int phycont_threshold, int cdlg_threshold, SceGxmMultisampleMode msaa);
GLboolean vglFramebufferMultisample(GLuint framebuffer, GLint samples); /* FBO_MSAA builds: 1, 2 or 4 samples for this framebuffer object */
''')
print('FBO_MSAA patch applied to', d)

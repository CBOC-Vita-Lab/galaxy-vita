#!/usr/bin/env python3
"""GPU_TIMERS for vitaGL (galaxy-base): the GPU completion time of every scene, after the Strikers port's rt.c (its GPU-timer
part only), made to live with the UNIFORM_PINGPONG patch of scripts/build-vitagl.sh.

    tools/vitagl/gpu_timers.py <vitaGL checkout>     patches <checkout>/source in place (use a copy; the build helper does)

Everything is compiled only with -DGPU_TIMERS (build-vitagl.sh extra option GPU_TIMERS); without it the source behaves as before.

What it adds (source/rt_gpu.c, hooks in gxm.c, shared.h, vitaGL.h):
  vglRtSetup(VGL_RT_GPU_TIMERS, priority, affinity)   every sceGxmEndScene of the GL context gets a fragment notification of its own
      (a ring of 64 words of the notification region, words 1..64) and a watcher thread waits for each scene's word in order
      (sceGxmNotificationWait: the word is only ever written by that scene, and is not reused before the watcher has passed it, so the
      wait cannot miss its value) and takes the time: the scene's GPU completion. Per frame (scenes between two vglSwapBuffers):
      busy = sum over scenes of (done - max(submitted, previous done)), span = last done - first submitted, tail = last done - last
      submitted. Work the GPU does on a scene before sceGxmEndScene returns is not seen as busy (the start is taken at submission); the
      watcher's wake-up latency is added to each done time.
  vglRtLastGpuFrame(&f)    the newest finished frame; returns the count of frames finished so far (0: none yet).
Living with query_fence (UNIFORM_PINGPONG gates the reuse of a uniform arena on it, occlusion queries wait on it): vitaGL counts
every scene in query_fence.value as before. A timed scene is ended with its own notification instead of query_fence, so the
watcher, once it has seen a timed scene complete, writes that scene's query_fence value into query_fence's word itself (only ever
upwards). The word therefore still says "every scene up to this value is done" - possibly a little later than the GPU would have
written it, never earlier: UNIFORM_PINGPONG and occlusion queries keep their meaning. (A GPU write of a later untimed scene's value
racing with the watcher's write can leave the word one step behind for a moment; that only delays, never advances, the verdict.)
Scenes the watcher cannot keep up with (more than 48 behind) are ended with query_fence as before and counted as untimed.
With the opt-in ring guard, a full metadata ring is not overwritten: excess scenes use query_fence without a timer record.
After any such drop, timer samples remain unavailable until restart rather than undercounting GPU work. Rendering does not wait
for the watcher. The host configures this before starting the watcher through vglRtSetRingGuard.

Per-scene vertex and fragment completion (GXV_GPU_SCENES in the back end; off until vglRtSetupScenes is called):
  vglRtSetupScenes(priority, affinity)   after vglRtSetup: every timed scene is also ended with a VERTEX notification of its own (a second
      ring of 64 words, words 65..128 of the notification region) and a second watcher thread stamps each scene's vertex completion the
      same way (sceGxmNotificationWait in order). The two watchers are independent, so a scene's vertex completion is seen while an earlier
      scene's fragment work still runs. Returns 1 when the watcher runs.
  vglRtTagScene(tag)    the host's tag for the scene that BEGINS next (scene_reset); a scene begun without one has tag 0.
  vglRtReadScenes(out, max)   the finished scenes (both watchers done with them) since the last call, oldest first, at most max:
      seq, frame, tag, begin (CPU time of the scene's begin), submitted, vertex done, fragment done (process time, us), timed, vtimed.
      A reader more than half the ring behind loses the oldest. One reader thread.
  Nothing else changes: the fragment watcher, the frame sums and query_fence work as before; with the scenes off no scene gets a vertex
  notification.

Also (only with -DANISO_SWITCH), the Strikers port's experimental anisotropic switch, OFF unless asked for:
  vglSetAnisotropicFilter(f)   f = 2 or 3: a trilinear texture (minification LINEAR with the mip filter on) gets this minification
      filter instead (the filter values vitasdk names MIPMAP_LINEAR / MIPMAP_POINT, believed to be the hardware's anisotropic modes;
      untested on a console, also in Strikers); 0: off (the default). The filter is written when a texture's filters are set
      (glTexParameteri of the minification filter, texture creation, a sampler change at a draw), not at every draw: a texture
      takes a change of this value the next time its minification filter is set (the back end sets it again for that, vgx_backend_gl.cpp,
      GXV_AUTOQ_ANISO). It may therefore be called while the game runs, from the thread that makes the GL calls.
"""
import sys

root = sys.argv[1].rstrip('/') + '/source'


def edit(path, pairs):
    p = root + '/' + path
    s = open(p).read()
    for a, b in pairs:
        assert s.count(a) == 1, (path, a, s.count(a))
        s = s.replace(a, b)
    open(p, 'w').write(s)


RT_GPU_C = r'''/*
 * rt_gpu.c (GPU_TIMERS builds): the GPU completion time of every scene. See tools/vitagl/gpu_timers.py in the Galaxy port for the
 * design; the watcher and the per-frame sums follow the Strikers port's rt.c.
 */
#include "shared.h"

#ifdef ANISO_SWITCH
uint32_t vgl_aniso_min = 0; // utils/gxm_utils.h vglSetTexMinFilter
void vglSetAnisotropicFilter(uint32_t sceFilter) { vgl_aniso_min = (sceFilter == 2 || sceFilter == 3) ? sceFilter : 0; }
#endif

#ifdef GPU_TIMERS
#define RT_SCENES 256
#define RT_SLOTS 64
#define RT_FRAMES 128
typedef struct {
	uint32_t frame;
	uint32_t submit_us;
	uint32_t done_us;
	uint32_t fence; // query_fence.value of the scene
	uint8_t timed;
	uint8_t vtimed; // (scene timers) a vertex notification of its own
	uint32_t seq, tag, begin_us;
	volatile uint32_t vdone_us;
} rt_scene_rec;
static rt_scene_rec rt_ring[RT_SCENES];
static volatile uint32_t rt_produced = 0; // scenes recorded
static volatile uint32_t rt_consumed = 0; // scenes the watcher has finished with
static volatile uint32_t rt_watcher_idle = 0;
static volatile unsigned int *rt_slots = NULL;
static SceUID rt_sema = -1, rt_thread = -1;
static uint32_t rt_frame = 0; // render thread: frames (vglSwapBuffers) so far
static vglRtGpuFrame rt_gpu_frames[RT_FRAMES];
static volatile uint32_t rt_gpu_frame_head = 0;
static volatile uint32_t rt_scenes_timed = 0, rt_scenes_untimed = 0, rt_scenes_late = 0;
static GLboolean rt_ring_guard = GL_FALSE;
static volatile uint32_t rt_ring_drops = 0;
GLboolean vgl_rt_gpu_on = GL_FALSE;
// scene timers (vglRtSetupScenes): the vertex watcher
static volatile uint32_t rt_vconsumed = 0;
static volatile uint32_t rt_vwatcher_idle = 0;
static volatile unsigned int *rt_vslots = NULL;
static SceUID rt_vsema = -1, rt_vthread = -1;
static volatile GLboolean rt_scenes_on = GL_FALSE;
static uint32_t rt_tag_next = 0, rt_tag_cur = 0, rt_begin_cur = 0, rt_read = 0;

static inline uint32_t rt_now(void) { return sceKernelGetProcessTimeLow(); }

void vglRtSetRingGuard(uint32_t enabled) { if (rt_thread < 0) rt_ring_guard = enabled != 0; }
uint32_t vglRtRingDrops(void) { return rt_ring_drops; }

// gxm.c scene_end, query_fence.value already counting this scene: sceGxmEndScene with the scene's own fragment notification.
// gxm.c scene_reset, right before a scene begins: the host's tag goes with it (scene timers)
void vgl_rt_scene_begin(void) {
	rt_tag_cur = rt_tag_next;
	rt_tag_next = 0;
	rt_begin_cur = rt_now();
}

void vglRtTagScene(uint32_t tag) { rt_tag_next = tag; }

void vgl_rt_end_scene(SceGxmContext *ctx) {
	const uint32_t seq = rt_produced;
	const GLboolean scenes = rt_scenes_on;
	if ((rt_ring_guard && seq - rt_consumed >= RT_SCENES) || (scenes && seq - rt_vconsumed >= RT_SCENES)) {
		// A watcher can still hold the oldest record while blocked in NotificationWait.
		// Replacing its fence can incorrectly certify future GPU work as complete.
		sceGxmEndScene(ctx, NULL, &query_fence);
		if (rt_ring_drops != UINT32_MAX) rt_ring_drops++;
		return;
	}
	rt_scene_rec *r = &rt_ring[seq % RT_SCENES];
	r->frame = rt_frame;
	r->done_us = 0;
	r->fence = query_fence.value;
	r->seq = seq;
	r->tag = rt_tag_cur;
	r->begin_us = rt_begin_cur;
	r->vdone_us = 0;
	// (scene timers) the vertex notification, under the same rule as the fragment one
	SceGxmNotification vn, *vnp = NULL;
	r->vtimed = 0;
	if (scenes && seq - rt_vconsumed < RT_SLOTS - 16) {
		vn.address = &rt_vslots[seq % RT_SLOTS];
		vn.value = seq + 1;
		*vn.address = 0;
		vnp = &vn;
		r->vtimed = 1;
	}
	// The watcher must not fall a whole slot ring behind: a slot word is rewritten RT_SLOTS scenes later.
	if (seq - rt_consumed < RT_SLOTS - 16) {
		SceGxmNotification n;
		n.address = &rt_slots[seq % RT_SLOTS];
		n.value = seq + 1;
		*n.address = 0;
		sceGxmEndScene(ctx, vnp, &n);
		r->timed = 1;
	} else {
		sceGxmEndScene(ctx, vnp, &query_fence);
		r->timed = 0;
	}
	r->submit_us = rt_now();
	__sync_synchronize();
	rt_produced = seq + 1;
	if (rt_watcher_idle) {
		rt_watcher_idle = 0;
		sceKernelSignalSema(rt_sema, 1);
	}
	if (scenes && rt_vwatcher_idle) {
		rt_vwatcher_idle = 0;
		sceKernelSignalSema(rt_vsema, 1);
	}
}

// gxm.c vglSwapBuffers, after the frame's last scene was ended
void vgl_rt_frame_end(void) {
	rt_frame++;
}

static int rt_watcher(SceSize args, void *argp) {
	uint32_t cur = 0xFFFFFFFF, first_submit = 0, last_submit = 0, last_done = 0, prev_done = 0, busy = 0, scenes = 0, timed = 0;
	GLboolean have_prev = GL_FALSE;
	for (;;) {
		const uint32_t produced = rt_produced;
		if (rt_consumed == produced) {
			rt_watcher_idle = 1;
			__sync_synchronize();
			if (rt_consumed == rt_produced)
				sceKernelWaitSema(rt_sema, 1, NULL);
			rt_watcher_idle = 0;
			continue;
		}
		__sync_synchronize();
		const uint32_t seq = rt_consumed;
		rt_scene_rec *r = &rt_ring[seq % RT_SCENES];
		if (r->frame != cur) {
			if (cur != 0xFFFFFFFF && scenes) {
				vglRtGpuFrame *f = &rt_gpu_frames[rt_gpu_frame_head % RT_FRAMES];
				f->busy_us = busy;
				f->span_us = timed ? last_done - first_submit : 0;
				f->tail_us = timed ? last_done - last_submit : 0;
				f->scenes = scenes;
				f->timed = timed;
				__sync_synchronize();
				rt_gpu_frame_head++;
			}
			cur = r->frame;
			first_submit = r->submit_us;
			busy = scenes = timed = 0;
		}
		scenes++;
		last_submit = r->submit_us;
		if (r->timed) {
			SceGxmNotification n;
			n.address = &rt_slots[seq % RT_SLOTS];
			n.value = seq + 1;
			if (*n.address != n.value)
				sceGxmNotificationWait(&n);
			else
				rt_scenes_late++;
			const uint32_t done = rt_now();
			r->done_us = done;
			// query_fence's word: every scene up to this one's value is done (only ever moved up; see gpu_timers.py)
			volatile unsigned int *qf = query_fence.address;
			if (qf && (int32_t)(r->fence - *qf) > 0)
				*qf = r->fence;
			uint32_t start = r->submit_us;
			if (have_prev && (int32_t)(prev_done - start) > 0)
				start = prev_done;
			if ((int32_t)(done - start) > 0)
				busy += done - start;
			prev_done = done;
			have_prev = GL_TRUE;
			last_done = done;
			timed++;
			rt_scenes_timed++;
		} else {
			rt_scenes_untimed++;
		}
		__sync_synchronize();
		rt_consumed = seq + 1;
	}
	return 0;
}

void vglRtSetup(uint32_t flags, int watcher_priority, int watcher_affinity) {
	if ((flags & VGL_RT_GPU_TIMERS) && rt_thread < 0) {
		rt_slots = (volatile unsigned int *)sceGxmGetNotificationRegion() + 1;
		for (int i = 0; i < RT_SLOTS; i++)
			rt_slots[i] = 0;
		rt_sema = sceKernelCreateSema("vgl_rt_sema", 0, 0, 0x7FFFFFFF, NULL);
		rt_thread = sceKernelCreateThread("vgl_rt_gpu", rt_watcher, watcher_priority ? watcher_priority : 0x10000100, 0x2000, 0, watcher_affinity, NULL);
		if (rt_sema < 0 || rt_thread < 0 || sceKernelStartThread(rt_thread, 0, NULL) < 0) {
			vgl_log("%s:%d GPU timer thread not started (0x%08X, 0x%08X).\n", __FILE__, __LINE__, rt_sema, rt_thread);
			rt_slots = NULL;
			return;
		}
		vgl_rt_gpu_on = GL_TRUE;
	}
}

// (scene timers) the vertex watcher: each scene's vertex completion, in order
static int rt_vwatcher(SceSize args, void *argp) {
	for (;;) {
		if (rt_vconsumed == rt_produced) {
			rt_vwatcher_idle = 1;
			__sync_synchronize();
			if (rt_vconsumed == rt_produced)
				sceKernelWaitSema(rt_vsema, 1, NULL);
			rt_vwatcher_idle = 0;
			continue;
		}
		__sync_synchronize();
		const uint32_t seq = rt_vconsumed;
		rt_scene_rec *r = &rt_ring[seq % RT_SCENES];
		if (r->vtimed) {
			SceGxmNotification n;
			n.address = &rt_vslots[seq % RT_SLOTS];
			n.value = seq + 1;
			if (*n.address != n.value)
				sceGxmNotificationWait(&n);
			r->vdone_us = rt_now();
		}
		__sync_synchronize();
		rt_vconsumed = seq + 1;
	}
	return 0;
}

// Called on the thread that ends the scenes (the render thread), after vglRtSetup.
uint32_t vglRtSetupScenes(int watcher_priority, int watcher_affinity) {
	if (!vgl_rt_gpu_on || !rt_slots)
		return 0;
	if (rt_vthread >= 0)
		return 1;
	rt_vslots = rt_slots + RT_SLOTS; // words 65..128 of the notification region
	for (int i = 0; i < RT_SLOTS; i++)
		rt_vslots[i] = 0;
	rt_vconsumed = rt_produced;
	rt_read = rt_produced;
	rt_vsema = sceKernelCreateSema("vgl_rt_vsema", 0, 0, 0x7FFFFFFF, NULL);
	rt_vthread = sceKernelCreateThread("vgl_rt_gpu_v", rt_vwatcher, watcher_priority ? watcher_priority : 0x10000100, 0x2000, 0, watcher_affinity, NULL);
	if (rt_vsema < 0 || rt_vthread < 0 || sceKernelStartThread(rt_vthread, 0, NULL) < 0) {
		vgl_log("%s:%d GPU scene timer thread not started (0x%08X, 0x%08X).\n", __FILE__, __LINE__, rt_vsema, rt_vthread);
		rt_vthread = -1;
		return 0;
	}
	__sync_synchronize();
	rt_scenes_on = GL_TRUE;
	return 1;
}

uint32_t vglRtReadScenes(vglRtGpuScene *out, uint32_t max) {
	if (!rt_scenes_on)
		return 0;
	uint32_t done = rt_consumed;
	const uint32_t vdone = rt_vconsumed;
	if ((int32_t)(vdone - done) < 0)
		done = vdone;
	__sync_synchronize();
	if ((int32_t)(done - rt_read) < 0)
		rt_read = done;
	if (done - rt_read > RT_SCENES / 2)
		rt_read = done - RT_SCENES / 2; // (fell behind: the oldest are lost)
	uint32_t n = 0;
	while (rt_read != done && n < max) {
		const rt_scene_rec *r = &rt_ring[rt_read % RT_SCENES];
		vglRtGpuScene s;
		s.seq = rt_read;
		s.frame = r->frame;
		s.tag = r->tag;
		s.begin_us = r->begin_us;
		s.submit_us = r->submit_us;
		s.vdone_us = r->vdone_us;
		s.fdone_us = r->done_us;
		s.timed = r->timed;
		s.vtimed = r->vtimed;
		s.pad0 = s.pad1 = 0;
		__sync_synchronize();
		if (r->seq == rt_read)
			out[n++] = s;
		rt_read++;
	}
	return n;
}

uint32_t vglRtLastGpuFrame(vglRtGpuFrame *out) {
	const uint32_t h = rt_gpu_frame_head;
	if (!h)
		return 0;
	__sync_synchronize();
	*out = rt_gpu_frames[(h - 1) % RT_FRAMES];
	// Lost records make busy time incomplete, so never offer it as a valid sample.
	if (rt_ring_guard && rt_ring_drops) out->timed = 0;
	return h;
}

void vglRtGpuCounters(uint32_t *timed, uint32_t *untimed, uint32_t *late, uint32_t *pending) {
	*timed = rt_scenes_timed;
	*untimed = rt_scenes_untimed;
	*late = rt_scenes_late;
	*pending = rt_produced - rt_consumed;
}
#endif
'''

open(root + '/rt_gpu.c', 'w').write(RT_GPU_C)

edit('gxm.c', [
    ('''	// Ends current gxm scene
	query_fence.value++;
	sceGxmEndScene(gxm_context, NULL, &query_fence);''', '''	// Ends current gxm scene
	query_fence.value++;
#ifdef GPU_TIMERS
	if (vgl_rt_gpu_on) // the scene's own fragment notification for the GPU timers (rt_gpu.c)
		vgl_rt_end_scene(gxm_context);
	else
#endif
	sceGxmEndScene(gxm_context, NULL, &query_fence);'''),
    ('''		// Starting drawing scene
		is_rendering_display = !active_write_fb;
''', '''#ifdef GPU_TIMERS
		if (vgl_rt_gpu_on) // the host's tag for the scene beginning now (rt_gpu.c, scene timers)
			vgl_rt_scene_begin();
#endif
		// Starting drawing scene
		is_rendering_display = !active_write_fb;
'''),
    ('''	if (!needs_scene_reset)
		scene_end();
''', '''	if (!needs_scene_reset)
		scene_end();
#ifdef GPU_TIMERS
	vgl_rt_frame_end(); // the frame boundary for the GPU timers (rt_gpu.c)
#endif
'''),
])

edit('shared.h', [
    ('''extern SceGxmNotification query_fence; // Fence used for occlusion queries sync''', '''extern SceGxmNotification query_fence; // Fence used for occlusion queries sync
#ifdef GPU_TIMERS
extern GLboolean vgl_rt_gpu_on; // rt_gpu.c
void vgl_rt_end_scene(SceGxmContext *ctx);
void vgl_rt_scene_begin(void);
void vgl_rt_frame_end(void);
#endif'''),
])

edit('utils/gxm_utils.h', [
    ("""static inline __attribute__((always_inline)) void vglSetTexMinFilter(SceGxmTexture *texture, SceGxmTextureFilter minFilter) {
	SceGxmTextureInternal *tex = (SceGxmTextureInternal *)texture;
""", """#ifdef ANISO_SWITCH
extern uint32_t vgl_aniso_min; // rt_gpu.c vglSetAnisotropicFilter
#endif
static inline __attribute__((always_inline)) void vglSetTexMinFilter(SceGxmTexture *texture, SceGxmTextureFilter minFilter) {
	SceGxmTextureInternal *tex = (SceGxmTextureInternal *)texture;
"""),
    # The substitution is made here, not in the minification setter: every caller sets the minification filter first and the mip
    # filter right after it (textures.c, ffp.c, custom_shaders.c), so only here are both the new minification filter and the new
    # mip filter known. A filter that is trilinear after the call (linear, mip filter on) becomes the anisotropic value; any other
    # call leaves the plain value the minification setter wrote. It therefore holds each time the filters are set again, which is
    # how the game switches it while running.
    ("""	tex->control_words[0] = mipFilter & 0x200 | tex->control_words[0] & 0xFFFFFDFF;
""", """	tex->control_words[0] = mipFilter & 0x200 | tex->control_words[0] & 0xFFFFFDFF;
#ifdef ANISO_SWITCH
	if (vgl_aniso_min && (mipFilter & 0x200) && ((tex->control_words[0] & 0xC00) >> 10) == SCE_GXM_TEXTURE_FILTER_LINEAR)
		tex->control_words[0] = (tex->control_words[0] & 0xFFFFF3FF) | ((vgl_aniso_min << 10) & 0xC00);
#endif
"""),
])

edit('vitaGL.h', [
    ('''void vglUseTripleBuffering(GLboolean usage);''', '''void vglUseTripleBuffering(GLboolean usage);
// ---- GPU timers (builds with GPU_TIMERS only: source/rt_gpu.c; the names follow the Strikers port's vitaGL) ----
#define VGL_RT_GPU_TIMERS 0x8 // a fragment notification per scene and a watcher thread (priority, affinity): GPU completion of every scene
void vglRtSetup(uint32_t flags, int watcher_priority, int watcher_affinity);
void vglRtSetRingGuard(uint32_t enabled); // configure before vglRtSetup; default off
uint32_t vglRtRingDrops(void); // dropped timing records; no rendering commands are dropped
typedef struct {
	uint32_t busy_us, span_us, tail_us; // GPU busy time, first submission to last completion, last submission to last completion
	uint16_t scenes, timed;
} vglRtGpuFrame;
uint32_t vglRtLastGpuFrame(vglRtGpuFrame *out); // the newest finished frame; returns frames finished so far (0: none)
void vglRtGpuCounters(uint32_t *timed, uint32_t *untimed, uint32_t *late, uint32_t *pending);
// scene timers (GXV_GPU_SCENES): a vertex notification per scene too, a second watcher, and the scenes one by one
typedef struct {
	uint32_t seq, frame, tag, begin_us, submit_us, vdone_us, fdone_us; // process time (us, low 32 bits)
	uint8_t timed, vtimed, pad0, pad1;
} vglRtGpuScene;
uint32_t vglRtSetupScenes(int watcher_priority, int watcher_affinity); // after vglRtSetup, on the thread that ends the scenes; 1: running
void vglRtTagScene(uint32_t tag); // the host's tag for the scene that begins next (0: none)
uint32_t vglRtReadScenes(vglRtGpuScene *out, uint32_t max); // finished scenes since the last call (one reader)
void vglSetAnisotropicFilter(uint32_t sceFilter); // builds with ANISO_SWITCH: 0 off (default); 2 or 3 for trilinear textures, experimental'''),
])
print('GPU_TIMERS patch applied to', root)

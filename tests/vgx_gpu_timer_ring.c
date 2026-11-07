#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef int SceUID;
typedef unsigned SceSize;
typedef struct { int unused; } SceGxmContext;
typedef struct { volatile unsigned int *address; uint32_t value; } SceGxmNotification;
typedef struct { uint32_t busy_us, span_us, tail_us; uint16_t scenes, timed; } vglRtGpuFrame;
typedef struct { uint32_t seq, frame, tag, begin_us, submit_us, vdone_us, fdone_us; uint8_t timed, vtimed, pad0, pad1; } vglRtGpuScene; // (scene timers)
#define GL_FALSE 0
#define GL_TRUE 1
#define VGL_RT_GPU_TIMERS 8
#define GPU_TIMERS

static unsigned int notifications[129]; // query_fence, 64 fragment slots, 64 vertex slots (scene timers)
static SceGxmNotification query_fence = { &notifications[0], 0 };
static unsigned submitted, gpu_completed, waits, inject_count, stopped;
static int drain;
static uint32_t tick;
static jmp_buf stop_watcher;

static uint32_t sceKernelGetProcessTimeLow(void) { return ++tick; }
static void *sceGxmGetNotificationRegion(void) { return notifications; }
static int sceKernelCreateSema(const char *s, int a, int b, int c, void *d) { return 1; }
static int sceKernelCreateThread(const char *s, int (*f)(SceSize, void *), int a, int b, int c, int d, void *e) { return 1; }
static int sceKernelStartThread(int t, int n, void *p) { return 0; }
static int sceKernelSignalSema(int s, int n) { return 0; }
static int sceKernelWaitSema(int s, int n, void *p) { stopped = 1; longjmp(stop_watcher, 1); }
static int sceGxmEndScene(SceGxmContext *c, void *v, const SceGxmNotification *n) { ++submitted; return 0; }
static void sceGxmNotificationWait(const SceGxmNotification *n);
#define vgl_log(...) ((void)0)

#include "rt_gpu_fixture.inc"

static void submit_scene(void) {
    ++query_fence.value;
    vgl_rt_end_scene(NULL);
}

static void sceGxmNotificationWait(const SceGxmNotification *n) {
    if (drain) {
        *n->address = n->value;
        gpu_completed = query_fence.value;
        return;
    }
    if (++waits == 2) {
        stopped = 2;
        longjmp(stop_watcher, 1);
    }
    // Deterministic interleaving: the render thread runs while the watcher waits.
    // Only the first scene completes; later GPU work remains in flight.
    for (unsigned i = 0; i < inject_count; ++i) submit_scene();
    *n->address = n->value;
    gpu_completed = 1;
}

// GXV_GPU_SCENES (gpu_timers.py vglRtSetupScenes / vglRtTagScene / vglRtReadScenes): tags follow the scene that begins after them,
// both watchers stamp every scene, the reader returns each finished scene once, in order, with its frame and tag.
static int scenes_test(void) {
    vglRtSetRingGuard(1);
    vglRtSetup(VGL_RT_GPU_TIMERS, 1, 1);
    assert(vglRtSetupScenes(1, 1) == 1);
    drain = 1;
    // frame 0: scenes tagged 11, 12; an untagged display scene; frame 1: one scene tagged 21
    const uint32_t tags[4] = {11, 12, 0, 21};
    for (int i = 0; i < 4; ++i) {
        if (tags[i]) vglRtTagScene(tags[i]);
        vgl_rt_scene_begin();
        ++query_fence.value;
        vgl_rt_end_scene(NULL);
        if (i == 2) vgl_rt_frame_end();
    }
    if (!setjmp(stop_watcher)) rt_vwatcher(0, NULL);
    vglRtGpuScene out[8];
    assert(vglRtReadScenes(out, 8) == 0); // (the fragment watcher has not seen them yet)
    if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
    const uint32_t n = vglRtReadScenes(out, 8);
    assert(n == 4);
    for (uint32_t i = 0; i < n; ++i) {
        assert(out[i].seq == i && out[i].tag == tags[i] && out[i].timed && out[i].vtimed);
        assert(out[i].frame == (i < 3 ? 0u : 1u));
        assert(out[i].vdone_us != 0 && out[i].fdone_us != 0 && out[i].begin_us != 0);
        assert((int32_t)(out[i].submit_us - out[i].begin_us) > 0);
    }
    assert(vglRtReadScenes(out, 8) == 0);
    assert(vglRtRingDrops() == 0);
    printf("PASS: scene timers: 4 scenes read once, in order, with their frames and tags\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "scenes")) return scenes_test();
    assert(argc == 3);
    inject_count = (unsigned)strtoul(argv[1], NULL, 10);
    const int guard = atoi(argv[2]);
#ifdef FIXTURE_HAS_RING_GUARD
    vglRtSetRingGuard(guard);
#else
    (void)guard;
#endif
    vglRtSetup(VGL_RT_GPU_TIMERS, 1, 1);
    submit_scene();
    if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
    printf("guard=%d injected=%u submitted=%u produced=%u consumed=%u gpu_completed=%u published_fence=%u stopped=%u\n",
           guard, inject_count, submitted, rt_produced, rt_consumed, gpu_completed, notifications[0], stopped);
    fflush(stdout);
    assert(stopped != 0);
    assert(submitted == inject_count + 1);
    if (guard) {
        assert(notifications[0] <= gpu_completed);
        assert(rt_produced - rt_consumed <= RT_SCENES);
#ifdef FIXTURE_HAS_RING_GUARD
        assert(vglRtRingDrops() == (inject_count >= RT_SCENES ? inject_count + 1 - RT_SCENES : 0));
        rt_gpu_frame_head = 1;
        rt_gpu_frames[0].scenes = 1;
        rt_gpu_frames[0].timed = 1;
        vglRtGpuFrame sample;
        assert(vglRtLastGpuFrame(&sample) == 1);
        assert(sample.timed == (vglRtRingDrops() ? 0 : 1));
        const uint32_t drops = vglRtRingDrops();
        drain = 1;
        gpu_completed = query_fence.value;
        notifications[0] = gpu_completed;
        if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
        assert(rt_consumed == rt_produced);
        const uint32_t before = rt_produced;
        submit_scene();
        assert(rt_produced == before + 1);
        if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
        assert(rt_consumed == rt_produced);
        assert(notifications[0] == gpu_completed);
        assert(vglRtRingDrops() == drops);
        assert(vglRtLastGpuFrame(&sample) == 1);
        assert(sample.timed == (drops ? 0 : 1));
        printf("PASS: ring drains and resumes recording without accepting incomplete timing; drops=%u\n", drops);
#endif
    } else if (inject_count >= RT_SCENES) {
        assert(notifications[0] > gpu_completed);
    } else {
        assert(notifications[0] <= gpu_completed);
    }
    return 0;
}

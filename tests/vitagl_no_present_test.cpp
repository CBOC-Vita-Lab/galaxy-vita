#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using GLboolean = int;
constexpr int GL_TRUE = 1, GL_FALSE = 0;
#define THREAD_SAFE()
static int gxm_context = 7;
static struct { unsigned value; } query_fence;
static bool system_app_mode = false;
static unsigned vsync_interval = 1;
static GLboolean needs_scene_reset, needs_end_scene, scene_not_flushed, dirty_framebuffer;
static uint32_t dirty_frag_unifs, dirty_vert_unifs;
static GLboolean dirty_shader_frag_unifs, dirty_shader_vert_unifs;
static unsigned frame = 73, circular = 2, front = 1, back = 2, gc = 0, display = 0;
static std::vector<int> calls;
static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void sceGxmEndScene(int c, void*, decltype(query_fence)*) { Require(c == 7, "correct context"); calls.push_back(1); }
static void sceGxmFinish(int c) { Require(c == 7, "correct context"); calls.push_back(2); }
static void sceDisplayWaitVblankStartMulti(unsigned) { calls.push_back(9); }
static void scene_reset() { calls.push_back(8); }
#ifdef GPU_TIMERS
static GLboolean vgl_rt_gpu_on;
static void vgl_rt_end_scene(int) { calls.push_back(3); }
#endif
#include "vitagl_no_present_functions.inc"
int main() {
    for (unsigned timed = 0; timed < 2; ++timed) {
#ifdef GPU_TIMERS
        vgl_rt_gpu_on = timed;
#endif
        for (bool active : {false, true}) {
            calls.clear();
            query_fence.value = 10;
            needs_end_scene = active;
            needs_scene_reset = !active;
            scene_not_flushed = GL_FALSE; // old glFinish misses work in this state
            dirty_frag_unifs = dirty_vert_unifs = 0;
            dirty_shader_frag_unifs = dirty_shader_vert_unifs = GL_FALSE;
            glFinish();
            Require(calls.empty(), "old glFinish false-flag case does not retire GPU work");
            vglSubmitWithoutSwap();
            Require(calls.size() == (active ? 2u : 1u) && calls.back() == 2, "unconditional finish follows any active scene end");
#ifdef GPU_TIMERS
            if (active) Require(calls.front() == (timed ? 3 : 1), "scene end uses timer-aware production wrapper");
#else
            if (active) Require(calls.front() == 1, "scene end precedes GPU finish");
#endif
            Require(query_fence.value == (active ? 11u : 10u), "scene fence advances only for an open scene");
            Require(!needs_end_scene && needs_scene_reset && !scene_not_flushed, "next scene resets without ending twice");
            Require(dirty_frag_unifs == 0xFFFFFFFF && dirty_vert_unifs == 0xFFFF &&
                    dirty_shader_frag_unifs && dirty_shader_vert_unifs, "next scene restores dirty uniforms");
            vglSubmitWithoutSwap();
            Require(calls.back() == 2 && calls.size() == (active ? 3u : 2u), "repeated call finishes but never re-ends a closed scene");
            Require(frame == 73 && circular == 2 && front == 1 && back == 2 && gc == 0 && display == 0,
                    "hidden retirement leaves display/pool/frame/GC ownership unchanged");
        }
    }
    std::puts("PASS: actual driver function text; open/closed/repeated retirement, false flush flag, timer wrapper; host stubs only");
}

// c183fix F7: the hrw41 depth mark (PORT_GX_MARK_SCENE_DEPTH, vgx_draw.cpp g_depthMarkNext) must end with its picture, also when the
// picture's buffer ends without a display copy (FrontPresent alone, or FrontFresh: the pair's partial fresh draw). Before the fix it was
// cleared only in QueueCopyDisp, so the next picture's first record carried the mark and the back end made that pair's mask at the wrong
// place. Runs the real front end (null back end) and reads depthMark on EFB copies (QueueCopyTex: the same flag as on draws).
//   tools/test_c183_depth_mark.sh
#include "vgx_state.h"
#include <cstdio>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
extern "C" uint32_t port_scene_key() { return 1; }
extern "C" int port_screen_quiet() { return 0; }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int port_perf_mode_60() { return 1; }
extern "C" void port_vgx2_boost(int) {}
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" int port_vita_thread_core3(int, int) { return 0; }
extern "C" void vgx_res_tex_note(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
#if defined(__APPLE__)
// Mach-O does not leave these ELF-style weak integration hooks unresolved.
// This fixture has no game clock, card I/O, second front end or GL backend.
struct Vgx2Obj;
extern "C" uint32_t port_camera_cut_count() { return 0; }
extern "C" int port_card_write_async(const char*, int, const void*, unsigned) { return 0; }
extern "C" int port_scene_loading_active() { return 0; }
extern "C" uint32_t port_scene_loading_key() { return 0; }
extern "C" uint32_t port_scene_serial() { return 0; }
extern "C" const char* port_stream_census_model_name(unsigned) { return nullptr; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void vgx2_b_picture_state(const void*, uint32_t, const void*, uint32_t, uint64_t) {}
extern "C" int vgx2_b_state_objs(Vgx2Obj*) { return 0; }
extern "C" void vgx2_b_thread_main() {}
extern "C" int vgx_be_tex_totals(uint64_t*, double*) { return 0; }
extern "C" int vgx_smooth_hold_active() { return 0; }
#endif

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
// Each buffer handed on: the depthMark of its EFB copies, in order.
static std::vector<std::vector<int>> g_bufs;
static void Sink(vgx::CmdBuf& b) {
    std::vector<int> marks;
    for (const vgx::CopyTexCmd& c : b.copyTex) marks.push_back(c.depthMark);
    g_bufs.push_back(marks);
}
static uint8_t g_dest[4096];
static void Copy() { vgx::QueueCopyTex(g_dest, false); }
static void EndWithDisplayCopy() {
    vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
    vgx::QueueCopyDisp(false);
    vgx::FrontPresent(false, nullptr);
}

int main() {
    vgx::ResetState();
    vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::g.texCopySrc = vgx::EfbRect{0, 0, 64, 64};
    vgx::g.texCopyDstW = 64;
    vgx::g.texCopyDstH = 64;
    vgx::QueueSetTarget();

    // control: inside one picture the mark covers every copy after the marker, and the display copy ends it
    g_bufs.clear();
    Copy();
    vgx::SceneDepthMarker();
    Copy();
    Copy();
    EndWithDisplayCopy();
    Copy();
    EndWithDisplayCopy();
    Check(g_bufs.size() == 2 && g_bufs[0] == std::vector<int>{0, 1, 1}, "the mark covers the copies after the marker, to the picture's end");
    Check(g_bufs.size() == 2 && g_bufs[1] == std::vector<int>{0}, "a display copy ends the mark");

    // a picture handed on without a display copy (FrontPresent alone)
    g_bufs.clear();
    vgx::SceneDepthMarker();
    Copy();
    vgx::FrontPresent(false, nullptr);
    Copy();
    EndWithDisplayCopy();
    Check(g_bufs.size() == 2 && g_bufs[0] == std::vector<int>{1}, "a picture without a display copy is marked after its marker");
    Check(g_bufs.size() == 2 && g_bufs[1] == std::vector<int>{0}, "... and its mark does not reach the next picture");

    // the pair's partial fresh draw (FrontFresh): its buffer ends without a display copy either
    g_bufs.clear();
    vgx::SceneDepthMarker();
    Copy();
    vgx::FrontFresh(7);
    Copy();
    EndWithDisplayCopy();
    Check(g_bufs.size() == 2 && g_bufs[0] == std::vector<int>{1}, "the fresh draw's buffer is marked after its marker");
    Check(g_bufs.size() == 2 && g_bufs[1] == std::vector<int>{0}, "... and the mark does not reach the next picture");

    std::printf("c183 depth mark: %s\n", g_fail ? "FAILED" : "PASS");
    return g_fail ? 1 : 0;
}

// Legacy diagnostic null backend; it does not render or implement EFB copies.
// Native presentation requires the actual recorder consumer, not its counter.
#include <atomic>

#include "gpu.h"
#include "port/port.h"

namespace {
struct NullBackend : gpu::Backend {
    uint64_t draws = 0;
    void draw(int, int, gpu::u32, const gpu::u8*, gpu::u32) override { draws++; }
    void bpWrite(gpu::u32, gpu::u32, gpu::u32) override {}
    void xfWrite(gpu::u32, gpu::u32) override {}
    void cpWrite(gpu::u32, gpu::u32) override {}
    void efbCopy(bool, bool) override {}
    void loadTlut(gpu::u32) override {}
    void preloadTexture() override {}
    gpu::u32 peekColor(gpu::u16, gpu::u16) override { return 0; }
    gpu::u32 peekZ(gpu::u16, gpu::u16) override { return 0x00FFFFFF; }
};
NullBackend sNull;
#if !defined(__vita__)
uint64_t sFrames;
#endif
}  // namespace

extern "C" void port_gx_use_null_backend(void) { gpu::setBackend(&sNull); }

#if !defined(__vita__)
extern std::atomic<uint64_t> gPresentedFrames;
#endif
extern "C" void port_gx_recorder_end_frame(void);
extern "C" void port_gx_recorder_present_xfb(const void*,int);

extern "C" int port_vgx_active(void);
extern "C" void __attribute__((weak)) port_gx_present_xfb(void* xfb, int black) {
    // vgx presents the frame itself, at the game's display copy.
    if (port_vgx_active()) return;
#if defined(__vita__)
    port_gx_recorder_present_xfb(xfb,black);
#else
    port_gx_recorder_end_frame();
    sFrames++;
    gPresentedFrames.store(sFrames);
    if ((sFrames % 300) == 1) {
        port_log("gx: frame %llu presented (xfb %p%s), %llu draws so far", (unsigned long long)sFrames, xfb, black ? ", black" : "",
                 (unsigned long long)sNull.draws);
    }
#endif
}

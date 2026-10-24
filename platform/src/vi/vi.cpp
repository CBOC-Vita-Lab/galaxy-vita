// Video interface: the retrace clock driving the game's frame pacing.
// The frame shown to the player is chosen by the renderer from the XFB the
// game flips to here.  In the headset the display times the retraces
// (port_vi_retrace, every second refresh at 120 Hz); otherwise, and as a
// fallback, a free-running 59.94 Hz clock does.
#include <string.h>

#include <atomic>
#include <thread>

#include "port/heap_routing.h"
#include "port/port.h"
#include "port/qemu_hooks.h"
#include "port/vi.h"
#include "revolution/os.h"
#include "revolution/vi.h"

static VIRetraceCallback sPreCb;
static VIRetraceCallback sPostCb;
static volatile u32 sRetraceCount;
static OSThreadQueue sRetraceQueue;
static void* sNextFb;
static void* sStagedFb;
static void* sCurrentFb;
static bool sFlushPending;
static bool sBlack = true;
static bool sPendingBlack = true;
static bool sFlushedBlack = true;
static PortViDisplay sStagedDisplay{},sFlushedDisplay{},sCurrentDisplay{};
static bool sInited;
static std::atomic<int> sPaused{0};
static std::atomic<int64_t> sExternalAt{0};  // host time of the last display-timed retrace
static std::atomic<int64_t> sRaisedAt{0};    // host time of the last retrace raised

// Raises the retrace interrupt, unless one was raised less than 8 ms ago: as
// on the console, retraces are a field apart.  When the display takes the
// retraces back from the fallback clock, the two could otherwise land a
// millisecond apart, and the game drew two frames within one retrace.
static void raiseRetrace() {
    int64_t now = port_host_time_ns();
    int64_t last = sRaisedAt.load();
    if (now - last < 8000000 || !sRaisedAt.compare_exchange_strong(last, now)) {
        return;
    }
    port_irq_raise(PORT_IRQ_VI);
}

// Implemented by the renderer: show the frame the game copied to `xfb`.
extern "C" void port_gx_present_xfb(void* xfb, int black);
extern "C" u8 SCGetAspectRatio(void);

static void viIrq() {
    sRetraceCount = sRetraceCount + 1;
    if (sPreCb) {
        sPreCb(sRetraceCount);
    }
    const bool flushed=sFlushPending;
    if (flushed) {
        sCurrentFb = sNextFb;
        sBlack = sFlushedBlack;
        sCurrentDisplay = sFlushedDisplay;
        sFlushPending = false;
    }
    // Native VI scans the current RAM again even without a register flush:
    // direct-print CPU writes must remain visible on an unchanged framebuffer.
    if (flushed
#if defined(__vita__)
        || (!sBlack && sCurrentDisplay.configured && sCurrentFb)
#endif
    ) port_gx_present_xfb(sCurrentFb, sBlack ? 1 : 0);
    if (sPostCb) {
        sPostCb(sRetraceCount);
    }
    OSWakeupThread(&sRetraceQueue);
}

static void viClock() {
    PORT_QEMU_THREAD("vi-clock");
    port_thread_class(1);
    // NTSC / EURGB60 field rate.
    const int64_t period = 16683350;  // ns (1001/60000 s)
    int64_t next = port_host_time_ns() + period;
    for (;;) {
        int64_t now = port_host_time_ns();
        if (next > now) {
            port_host_sleep_ns(next - now);
        }
        next += period;
        now = port_host_time_ns();
        if (now > next + period * 4) {
            next = now + period;  // resync after a long stall instead of bursting
        }
        if (sPaused.load()) {
            continue;  // no retrace: the game stays in VIWaitForRetrace
        }
        if (now - sExternalAt.load() < period * 4) {
            continue;  // the display times the retraces
        }
        raiseRetrace();
    }
}

extern "C" {

void port_vi_retrace(void) {
    sExternalAt.store(port_host_time_ns());
    if (!sPaused.load()) {
        raiseRetrace();
    }
}

void port_set_paused(int paused) {
    if (sPaused.exchange(paused ? 1 : 0) != (paused ? 1 : 0)) {
        port_log("game %s", paused ? "paused" : "resumed");
    }
}

int port_is_paused(void) { return sPaused.load(); }

void VIInit(void) {
    if (sInited) {
        return;
    }
    sInited = true;
    OSInitThreadQueue(&sRetraceQueue);
    port_irq_set_handler(PORT_IRQ_VI, viIrq);
    PortHostAllocScope scope;
    std::thread(viClock).detach();
}

static void displayHeight() {
    const unsigned scan=sStagedDisplay.tv_mode&3;
    sStagedDisplay.display_height=sStagedDisplay.pan_height *
        ((scan==VI_PROGRESSIVE || scan==VI_3D || sStagedDisplay.xfb_mode!=VI_XFBMODE_SF) ? 1 : 2);
}
void VIConfigure(const GXRenderModeObj* rm) {
    if (!rm) port_fatal("VIConfigure requires an original render mode");
    BOOL en = OSDisableInterrupts();
    sStagedDisplay={1,uint32_t(rm->viTVmode),rm->fbWidth,rm->xfbHeight,uint32_t(rm->xFBmode),rm->field_rendering,
        0,0,rm->fbWidth,rm->xfbHeight,rm->viXOrigin,
        uint32_t(rm->viYOrigin)*((uint32_t(rm->viTVmode)&3)==VI_NON_INTERLACE ? 2 : 1),rm->viWidth,0,SCGetAspectRatio()};
    displayHeight();
    OSRestoreInterrupts(en);
}
void VIConfigurePan(u16 x,u16 y,u16 width,u16 height) {
    BOOL en = OSDisableInterrupts();
    sStagedDisplay.pan_x=x; sStagedDisplay.pan_y=y;
    sStagedDisplay.pan_width=width; sStagedDisplay.pan_height=height;
    displayHeight();
    OSRestoreInterrupts(en);
}

void port_vi_display_config(PortViDisplay* out) { if (out) *out=sCurrentDisplay; }

void VIFlush(void) {
    BOOL en = OSDisableInterrupts();
    sNextFb = sStagedFb;
    sFlushedBlack = sPendingBlack;
    sFlushedDisplay = sStagedDisplay;
    sFlushPending = true;
    OSRestoreInterrupts(en);
}

void VISetNextFrameBuffer(void* fb) {
    BOOL en = OSDisableInterrupts();
    sStagedFb = fb;
    OSRestoreInterrupts(en);
}

void* VIGetNextFrameBuffer(void) { return sNextFb; }
void* VIGetCurrentFrameBuffer(void) { return sCurrentFb; }

void VIWaitForRetrace(void) {
    BOOL en = OSDisableInterrupts();
    u32 count = sRetraceCount;
    do {
        OSSleepThread(&sRetraceQueue);
    } while (count == sRetraceCount);
    OSRestoreInterrupts(en);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    BOOL en = OSDisableInterrupts();
    VIRetraceCallback old = sPreCb;
    sPreCb = cb;
    OSRestoreInterrupts(en);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    BOOL en = OSDisableInterrupts();
    VIRetraceCallback old = sPostCb;
    sPostCb = cb;
    OSRestoreInterrupts(en);
    return old;
}

void VISetBlack(BOOL black) {
    BOOL en = OSDisableInterrupts();
    sPendingBlack = black ? true : false;
    OSRestoreInterrupts(en);
}

u32 VIGetRetraceCount(void) { return sRetraceCount; }
u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetTvFormat(void) { return VI_EURGB60; }
u32 VIGetDTVStatus(void) { return 1; }  // component/digital cable: allow progressive
BOOL VIEnableDimming(BOOL enable) {
    (void)enable;
    return FALSE;
}
// Retraces left before the console dims the picture against burn-in (five
// minutes without input).  Never 0 here: at 0 the game takes the screen saver
// to be on and keeps the music at a third of its volume (AudSystem::
// screenSaverProcess).
u32 VIGetDimmingCount(void) { return 18000; }
void VISetTrapFilter(VIBool) {}

}  // extern "C"

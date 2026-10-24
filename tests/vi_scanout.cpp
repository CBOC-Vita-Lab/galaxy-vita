#include "revolution/os.h"
#include "revolution/vi.h"
#include "port/port.h"
#include "port/vi.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

static PortIrqHandler retrace;
static std::atomic<bool> clockStarted{false},clockParked{false};
static void* presented;
static int presentedBlack;
static unsigned presentations;
static uint8_t aspect=1;
static void (*presenter)(void*,int);
static void check(bool value,const char* message) {
    if (!value) { std::fprintf(stderr,"VI scanout: %s\n",message); std::exit(1); }
}
// Only the clock/IRQ/OS transports are doubled. The real VI copies, flushes
// and retrace handler are linked into the same native-worker fixture.
extern "C" {
BOOL OSDisableInterrupts() { return TRUE; }
BOOL OSRestoreInterrupts(BOOL value) { return value; }
void OSInitThreadQueue(OSThreadQueue*) {}
void OSWakeupThread(OSThreadQueue*) {}
void OSSleepThread(OSThreadQueue*) { std::abort(); }
void port_irq_set_handler(int irq,PortIrqHandler handler) {
    check(irq==PORT_IRQ_VI,"only VI registers the retrace handler"); retrace=handler;
}
void port_host_sleep_ns(int64_t) {
    if (clockStarted.load()) {
        clockParked=true;
        // VI's clock is process-lifetime. Its isolated fixture child exits
        // only after every actual native GPU owner has been retired.
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
u8 SCGetAspectRatio() { return aspect; }
void port_gx_present_xfb(void* xfb,int black) {
    presented=xfb; presentedBlack=black; ++presentations;
    if (presenter) presenter(xfb,black);
}
}

void testViFlush() {
    clockStarted=true; VIInit();
    while (!clockParked.load()) std::this_thread::yield();
    check(retrace,"real VI installs its retrace handler");
    unsigned char first[32]{},second[32]{};
    VISetNextFrameBuffer(first); VISetBlack(FALSE); VIFlush();
    VISetNextFrameBuffer(second); VISetBlack(TRUE);
    check(VIGetNextFrameBuffer()==first,"next framebuffer is latched by VIFlush, not by later writes");
    retrace();
    check(VIGetCurrentFrameBuffer()==first && presented==first && !presentedBlack && presentations==1,
        "retrace commits exactly the flushed framebuffer and black state");
    retrace(); check(presentations==1,"unchanged VI state does not issue a fabricated new presentation");
    VIFlush(); retrace();
    check(VIGetCurrentFrameBuffer()==second && presented==second && presentedBlack && presentations==2,
        "later staged framebuffer becomes current only after its own flush and retrace");
    VISetNextFrameBuffer(nullptr); VIFlush(); retrace();
    GXRenderModeObj mode{};
    mode.viTVmode=VI_TVMODE_EURGB60_PROG; mode.fbWidth=640; mode.xfbHeight=456;
    mode.xFBmode=VI_XFBMODE_SF; mode.viXOrigin=17; mode.viYOrigin=12; mode.viWidth=686; mode.viHeight=1;
    VIConfigure(&mode); VIFlush();
    mode.fbWidth=320; mode.viWidth=600; VIConfigure(&mode); VIConfigurePan(2,3,10,20);
    retrace(); PortViDisplay current{}; port_vi_display_config(&current);
    check(current.configured && current.fb_width==640 && current.fb_height==456 && current.pan_x==0 &&
        current.pan_y==0 && current.pan_width==640 && current.pan_height==456 && current.display_width==686 &&
        current.display_height==456 && current.origin_x==17 && current.origin_y==12 && current.aspect==1,
        "VI owns original mode values and commits only the flushed configuration, not later pan/mode writes");
    VIFlush(); retrace(); port_vi_display_config(&current);
    check(current.fb_width==320 && current.display_width==600 && current.pan_x==2 && current.pan_y==3 &&
        current.pan_width==10 && current.pan_height==20 && current.display_height==20,
        "VIConfigurePan changes the original source crop and derives progressive output height");
    std::fprintf(stderr,"VI: original framebuffer, black, mode and pan flush/retrace checks passed\n");
}

void configureTestVi(uint32_t width,uint32_t height,uint32_t panX,uint32_t panY,
                     uint32_t panWidth,uint32_t panHeight,void* xfb,uint32_t ratio,uint32_t tvMode,
                     uint32_t xfbMode,uint32_t field) {
    GXRenderModeObj mode{};
    mode.viTVmode=VITVMode(tvMode); aspect=ratio;
    mode.fbWidth=width; mode.efbHeight=height; mode.xfbHeight=height;
    mode.viXOrigin=17; mode.viYOrigin=12; mode.viWidth=686; mode.viHeight=1;
    mode.xFBmode=VIXFBMode(xfbMode); mode.field_rendering=field;
    VIConfigure(&mode); VIConfigurePan(panX,panY,panWidth,panHeight);
    VISetNextFrameBuffer(xfb); VISetBlack(FALSE); VIFlush();
    mode.fbWidth=999; // VIConfigure must own a value, not this stack pointer.
    retrace();
}

void setTestViPresenter(void (*callback)(void*,int)) { presenter=callback; }
void retraceTestVi() { retrace(); }

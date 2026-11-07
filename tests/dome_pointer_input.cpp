#include "input.h"
#include "revolution/kpad.h"
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static SceCtrlData raw;
static SceTouchData front;
static KPADStatus kpad;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* out, int) { *out=raw; return 1; }
int sceCtrlPeekBufferPositive2(int, SceCtrlData* out, int) { *out=raw; return 1; }
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* out) { *out={}; out->port[0]=SCE_CTRL_TYPE_PHY; return 0; }
int sceMotionStartSampling() { return -1; }
int sceMotionStopSampling() { return 0; }
int sceMotionGetState(SceMotionState*) { return -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* out) {
    *out={}; out->maxDispX=1919; out->maxDispY=port==SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* out, SceUInt32) { *out=port==SCE_TOUCH_PORT_FRONT ? front : SceTouchData{}; return 1; }
int port_pointer_on_menus() { return 0; }
int port_zoom_menu_active() { return 0; }
int port_zoom_adjust(int) { return 15; }
int port_rear_aim_enabled() { return 0; }
int port_rear_aim_toggle() { return 0; }
int port_star_bit_reach_next() { return 1; }
int port_stick_pointer_enabled() { return 0; }
int port_stick_pointer_toggle() { return 0; }
int64_t port_host_time_ns() { return 1; }
int port_skip_inject_a() { return -1; }
int port_handheld() { return 1; }
unsigned long long port_gx_recording_frame() { return 1; }
void port_log(const char*, ...) {}
}
static PortPadState poll() {
    PortPadState pad;
    port_vita_input_poll(.05f,&pad);
    port_input_set(0,&pad);
    assert(KPADRead(0,&kpad,1)==1);
    assert(kpad.pos.x==pad.pointerX && kpad.pos.y==pad.pointerY);
    return pad;
}
int main(int argc, char** argv) {
    assert(argc==4);
    setenv("GXV_REMAP",argv[1],1);
    setenv("GXV_VITA_WIDE",argv[2],1);
    setenv("PETARI_TOUCH_A",argv[3],1);
    const bool wide=std::atoi(argv[2]), touchA=std::atoi(argv[3]);
    raw.lx=raw.ly=raw.rx=raw.ry=128;
    assert(port_vita_input_init()); poll();
    for (int ix=0;ix<5;++ix) for (int iy=0;iy<5;++iy) {
        front.reportNum=1; front.report[0].x=1919*ix/4; front.report[0].y=1087*iy/4;
        const auto pad=poll();
        const float u=front.report[0].x/1919.0f, v=front.report[0].y/1087.0f;
        const float width=wide ? 1 : (544.0f*4/3)/960;
        const float x=std::fmin(1,std::fmax(-1,(u-.5f)/(.5f*width)));
        assert(std::fabs(pad.pointerX-x)<1e-6 && std::fabs(pad.pointerY-(2*v-1))<1e-6);
        assert(pad.pointerValid && kpad.dpd_valid_fg==2);
        front={}; for (int i=0;i<4;++i) poll();
    }
    // The real A edge arrives after aiming, whether supplied by Cross or by
    // an unmoved touch held for 0.2 seconds. A later Cross while touch-A is
    // already held cannot generate a second edge; this is expected OR behavior.
    front.reportNum=1; front.report[0].x=960; front.report[0].y=100;
    poll(); assert(!(kpad.trig&0x800));
    raw.buttons=SCE_CTRL_CROSS; poll(); assert(kpad.trig&0x800);
    raw.buttons=0; front={}; for (int i=0;i<5;++i) poll();
    front.reportNum=1; front.report[0].x=960; front.report[0].y=100;
    int edges=0; for (int i=0;i<8;++i) { poll(); edges+=(kpad.trig&0x800)!=0; }
    assert(edges==(touchA ? 1 : 0));
    raw.buttons=SCE_CTRL_CROSS; poll();
    assert(bool(kpad.trig&0x800)==!touchA);
    raw.buttons=0; front={}; for (int i=0;i<5;++i) poll();
    raw.buttons=SCE_CTRL_CROSS; poll(); assert(kpad.trig&0x800);
    port_vita_input_shutdown();
    std::printf("PASS: production touch/KPAD coordinates and A edges remap=%s wide=%s touchA=%s\n",argv[1],argv[2],argv[3]);
}

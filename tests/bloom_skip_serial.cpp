// PETARI_BLOOM_SKIP host proof, GX side (tools/test_bloom_skip.sh): the production GX units count every command that can write the
// EFB (GXBegin, GXCallDisplayList and its retained form, GXCopyTex, GXCopyDisp) in ONE counter (port_gx_draw_serial; the units each
// carry a weak definition), and no state-only call moves it. BloomEffect (decomp Game/Screen/BloomEffect.cpp) leaves its glow chain
// out only when the counter did not move between its black fill and postDraw, so a missed writer here would be a wrong picture there.
#define PORT_GX_FIFO_BATCH_OWNER 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/mtx.h"
#include "revolution/gx/GXRegs.h"

extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
int __PortGXFifoBatch;
static GXData gxData;
GXData* const __GXData = &gxData;
void __GXSetDirtyState(void) {}
int __PortGXFifoXfBurst12(const float*) { return 0; }
void GXResetWriteGatherPipe(void) {}
u32 port_host_to_phys(const void* p) { return (u32)(uintptr_t)p; }
u32 port_gx_draw_serial(void);
void port_gx_call_retained(const void* list, u32 nbytes, u32 handle);
int __PortGXRetainedOk(void) { return 1; }
void __PortGXFifoWriteBytes(const void* src, uint32_t n) {
    memcpy(__PortGXFifoPtr, src, n);
    __PortGXFifoPtr += n;
}
}
static uint8_t fifo[1 << 20];
extern "C" void __PortGXFifoOverflow(void) { __PortGXFifoPtr = fifo; }

static int fails = 0;
static void expect(const char* what, u32 before, u32 delta) {
    const u32 now = port_gx_draw_serial();
    if (now - before != delta) {
        printf("FAIL %s: counter moved by %u, expected %u\n", what, (unsigned)(now - before), (unsigned)delta);
        ++fails;
    } else {
        printf("ok   %s: +%u\n", what, (unsigned)delta);
    }
}

int main() {
    __PortGXFifoPtr = fifo;
    __PortGXFifoLimit = fifo + sizeof fifo - 4096;
    static u8 list[64] __attribute__((aligned(32)));
    static u8 tex[64 * 64 * 4] __attribute__((aligned(32)));
    memset(list, 0, sizeof list);
    u32 s;
    s = port_gx_draw_serial(); GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4); GXPosition3f32(0, 0, 0); GXPosition3f32(1, 0, 0); GXPosition3f32(0, 1, 0); GXPosition3f32(1, 1, 0); GXEnd();
    expect("GXBegin", s, 1);
    s = port_gx_draw_serial(); GXCallDisplayList(list, 32);
    expect("GXCallDisplayList", s, 1);
    s = port_gx_draw_serial(); port_gx_call_retained(list, 32, 7);
    expect("port_gx_call_retained (retained form)", s, 1);
    s = port_gx_draw_serial(); port_gx_call_retained(list, 32, 0);
    if (port_gx_draw_serial() == s) { printf("FAIL port_gx_call_retained (plain form) did not count\n"); ++fails; } else printf("ok   port_gx_call_retained (plain form): counted\n");
    s = port_gx_draw_serial(); GXSetTexCopySrc(0, 0, 64, 64); GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
    expect("GXSetTexCopySrc/Dst (state)", s, 0);
    s = port_gx_draw_serial(); GXCopyTex(tex, GX_FALSE);
    expect("GXCopyTex", s, 1);
    s = port_gx_draw_serial(); GXCopyTex(tex, GX_TRUE);
    expect("GXCopyTex with clear", s, 1);
    s = port_gx_draw_serial(); GXCopyDisp(tex, GX_TRUE);
    expect("GXCopyDisp", s, 1);
    u8 samples[12][2];
    memset(samples, 6, sizeof samples);
    u8 vfilter[7] = {0, 0, 21, 22, 21, 0, 0};
    Mtx m;
    memset(m, 0, sizeof m);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    s = port_gx_draw_serial(); GXLoadPosMtxImm(m, GX_PNMTX0); GXSetCurrentMtx(GX_PNMTX0); GXSetScissor(0, 0, 640, 456); GXSetViewport(0, 0, 640, 456, 0, 1);
    GXSetCopyClear(GXColor{0, 0, 0, 255}, 0xFFFFFF); GXSetCopyFilter(GX_FALSE, samples, GX_TRUE, vfilter);
    expect("matrix / scissor / viewport / copy-state calls", s, 0);
    u32 l, t, w, h;
    GXGetScissor(&l, &t, &w, &h);
    f32 vp[6];
    GXGetViewportv(vp);
    if (l != 0 || t != 0 || w != 640 || h != 456 || vp[2] != 640.0f || vp[3] != 456.0f) { printf("FAIL scissor/viewport read back %u %u %u %u / %g %g\n", l, t, w, h, vp[2], vp[3]); ++fails; }
    else printf("ok   GXGetScissor / GXGetViewportv read back the whole EFB\n");
    printf(fails ? "bloom_skip_serial: FAIL (%d)\n" : "bloom_skip_serial: PASS\n", fails);
    return fails ? 1 : 0;
}

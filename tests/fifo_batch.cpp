// GXV_FIFO_BATCH host proof (tools/test_fifo_batch.sh): every batched GX command writer gives the same bytes, the same final
// write pointer and the same __PortGXFifoOverflow calls (at the same write positions) as the element-wise form, for every
// start position and limit distance up to past the longest command. The overflow stub acts like the player's publish step
// (the writes continue in another buffer with a new limit), so a command that straddles the limit is covered too.
#define PORT_GX_FIFO_BATCH_OWNER 1
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/mtx.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/gx/GXVert.h"
#include "JSystem/J3DGraphBase/J3DFifo.hpp"

extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
int __PortGXFifoBatch;
static GXData gxData;
GXData* const __GXData = &gxData;
void __GXSetDirtyState(void) {}
int __PortGXFifoXfBurst12(const float*) { return 0; }  // (GXV_XFBURST has its own proof: tests/xfburst.cpp)
void GXResetWriteGatherPipe(void) {}
u32 port_host_to_phys(const void* p) { return (u32)(uintptr_t)p; }  // (identity: only the bytes written matter here)
}

static uint8_t buf[2][4096];
static int cur;
static std::vector<std::string> events;
extern "C" void __PortGXFifoOverflow(void) {
    events.push_back("ovf@" + std::to_string(cur) + ":" + std::to_string((int)(__PortGXFifoPtr - buf[cur])));
    cur ^= 1;
    __PortGXFifoPtr = buf[cur];
    __PortGXFifoLimit = buf[cur] + 64;
}

enum { kCommands = 31 };
static void command(int which, uint32_t seed) {
    const uint32_t a = seed * 2654435761u, b = seed ^ 0x9E3779B9u;
    const f32 f0 = (f32)(int32_t)a / 7.0f, f1 = -(f32)b / 3.0f, f2 = 0.0f;
    Mtx m;
    Mtx33 m3;
    for (int i = 0; i < 12; i++) ((f32*)m)[i] = (f32)(int32_t)(a * (i + 3)) / 1000.0f;
    for (int i = 0; i < 9; i++) ((f32*)m3)[i] = -(f32)(b * (i + 7)) / 77.0f;
    GXLightObj lo;
    memset(&lo, 0, sizeof lo);
    GXInitLightPos(&lo, m[0][0], m[0][1], m[0][2]);
    GXInitLightDir(&lo, m[1][0], m[1][1], m[1][2]);
    GXInitLightAttn(&lo, m[2][0], m[2][1], m[2][2], m[0][3], m[1][3], m[2][3]);
    GXColor c = {(u8)a, (u8)b, 3, 4};
    GXInitLightColor(&lo, c);
    switch (which) {
    case 0: GX_WRITE_RA_REG(a); break;
    case 1: GX_WRITE_XF_REG(0x1008 + (a & 0xF), b, 0); break;
    case 2: GX_WRITE_CP_STRM_REG(0x50, a & 7, b); break;
    case 3: J3DFifoLoadIndx(0x20, (u16)a, (u16)b); break;
    case 4: J3DFifoLoadCPCmd((u8)a, b); break;
    case 5: J3DFifoLoadNrmMtxIndx3x3((u16)a, b & 0xFF); break;
    case 6: J3DFifoLoadXFCmdHdr((u16)a, (u8)(b | 1)); break;
    case 7: J3DFifoWriteXFCmdHdr((u16)b, (u8)(a | 1)); break;
    case 8: J3DFifoLoadBPCmd(b); break;
    case 9: GXPosition3f32(f0, f1, f2); break;
    case 10: GXPosition2f32(f0, f1); break;
    case 11: GXPosition3s16((s16)a, (s16)b, (s16)(a >> 16)); break;
    case 12: GXPosition3u8((u8)a, (u8)b, (u8)(a >> 8)); break;
    case 13: GXNormal3f32(f1, f2, f0); break;
    case 14: GXColor4u8((u8)a, (u8)(a >> 8), (u8)b, (u8)(b >> 8)); break;
    case 15: GXTexCoord2f32(f1, f0); break;
    case 16: GXTexCoord2u16((u16)a, (u16)b); break;
    case 17: GXTexCoord2s16((s16)b, (s16)a); break;
    case 18: GXPosition3f32(f2, f0, f1); break;
    case 19: J3DFifoLoadPosMtxImm(m, seed % 10); break;
    case 20: J3DFifoLoadNrmMtxImm(m, seed % 10); break;
    case 21: J3DFifoLoadNrmMtxImm3x3(m3, seed % 10); break;
    case 22: J3DFifoLoadNrmMtxToTexMtx(m, seed % 10); break;
    case 23: J3DFifoLoadNrmMtxToTexMtx3x3(m3, seed % 10); break;
    case 24: GXLoadLightObjImm(&lo, (GXLightID)(1 << (seed % 8))); break;
    case 25: GXLoadNrmMtxImm(m, seed % 10); break;
    case 26: GXLoadPosMtxImm(m, seed % 10); break;
    case 27: GXLoadTexMtxImm(m, 30 + seed % 10 * 3, (seed & 1) ? GX_MTX3x4 : GX_MTX2x4); break;
    case 28: GXCallDisplayList((void*)(uintptr_t)(0x1000 * seed), 32 * seed); break;
    case 29: GXBegin(GX_TRIANGLES, GX_VTXFMT1, (u16)(seed * 3)); break;
    case 30: GXSetArray(GX_VA_POS, (void*)(uintptr_t)(0x80001000u + 32 * seed), (u8)seed); break;
    }
}

static void run(int batch, int start, int lim, int which, uint32_t seed) {
    __PortGXFifoBatch = batch;
    cur = 0;
    events.clear();
    memset(buf, 0xEE, sizeof buf);
    memset(&gxData, 0, sizeof gxData);
    *(u32*)&gxData.vNumNot = 1;  // (no flush primitive before GXBegin / GXCallDisplayList)
    __PortGXFifoPtr = buf[0] + start;
    __PortGXFifoLimit = buf[0] + lim;
    command(which, seed);
    events.push_back("end@" + std::to_string(cur) + ":" + std::to_string((int)(__PortGXFifoPtr - buf[cur])));
}

int main() {
    long cases = 0, bad = 0;
    for (int which = 0; which < kCommands; which++)
        for (int lim = 1; lim < 100; lim++)
            for (int start = 0; start < lim; start++)
                for (uint32_t seed = 1; seed < 4; seed++) {
                    run(0, start, lim, which, seed);
                    const std::vector<std::string> e0 = events;
                    const std::vector<uint8_t> b0(&buf[0][0], &buf[0][0] + sizeof buf);
                    run(1, start, lim, which, seed);
                    const std::vector<uint8_t> b1(&buf[0][0], &buf[0][0] + sizeof buf);
                    ++cases;
                    if (e0 != events || b0 != b1) {
                        if (++bad < 8) printf("mismatch: command %d, limit %d, start %d, seed %u\n", which, lim, start, seed);
                    }
                }
    printf("fifo batch: %ld cases, %ld mismatches\n", cases, bad);
    return bad != 0;
}

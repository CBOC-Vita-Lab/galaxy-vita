// Stubs for tests/jpa_look.cpp (tools/test_jpa_look.sh): GX state calls and the parts of JPA the check never reaches. Plain C++.
#include <stdint.h>
#include <stdlib.h>

// The GX command FIFO the JPA draw writes vertices into: a scratch buffer that wraps.
static uint8_t sFifo[1 << 16];
extern "C" {
uint8_t* __PortGXFifoPtr = sFifo;
uint8_t* __PortGXFifoLimit = sFifo + sizeof(sFifo) - 256;
void __PortGXFifoOverflow(void) {
    __PortGXFifoPtr = sFifo;
}
unsigned port_mtx_profile_begin(const void*) {
    return 0;
}
void port_mtx_profile_end(unsigned, unsigned) {
}
#define GX_STUB(name) \
    void name() {     \
    }
GX_STUB(GXBegin)
GX_STUB(GXCallDisplayList)
GX_STUB(GXLoadPosMtxImm)
GX_STUB(GXLoadTexMtxImm)
GX_STUB(GXSetAlphaCompare)
GX_STUB(GXSetArray)
GX_STUB(GXSetBlendMode)
GX_STUB(GXSetClipMode)
GX_STUB(GXSetIndTexCoordScale)
GX_STUB(GXSetIndTexMtx)
GX_STUB(GXSetIndTexOrder)
GX_STUB(GXSetLineWidth)
GX_STUB(GXSetMisc)
GX_STUB(GXSetNumIndStages)
GX_STUB(GXSetNumTevStages)
GX_STUB(GXSetNumTexGens)
GX_STUB(GXSetPointSize)
GX_STUB(GXSetTevAlphaIn)
GX_STUB(GXSetTevAlphaOp)
GX_STUB(GXSetTevColor)
GX_STUB(GXSetTevColorIn)
GX_STUB(GXSetTevColorOp)
GX_STUB(GXSetTevDirect)
GX_STUB(GXSetTevIndirect)
GX_STUB(GXSetTevOrder)
GX_STUB(GXSetTexCoordGen2)
GX_STUB(GXSetVtxDesc)
GX_STUB(GXSetZCompLoc)
GX_STUB(GXSetZMode)
}

// C++ symbols, declared with the game's signatures (same mangled names).
enum _GXTexMapID {};
struct JPAEmitterWorkData;
class JPABaseParticle;
class JKRHeap {
public:
    static void* alloc(unsigned int size, int, JKRHeap*);
};
void* JKRHeap::alloc(unsigned int size, int, JKRHeap*) {
    return malloc(size);
}
class JPAKeyBlock {
public:
    float calc(float);
};
float JPAKeyBlock::calc(float) {
    return 0.0f;
}
class JUTTexture {
public:
    void load(_GXTexMapID);
};
void JUTTexture::load(_GXTexMapID) {
}
void JPALoadExTex(JPAEmitterWorkData*) {
}
void JPADrawDBillboard(JPAEmitterWorkData*, JPABaseParticle*) {
}
void JPADrawDirection(JPAEmitterWorkData*, JPABaseParticle*) {
}
void JPADrawRotDirection(JPAEmitterWorkData*, JPABaseParticle*) {
}
void JPADrawRotYBillboard(JPAEmitterWorkData*, JPABaseParticle*) {
}
void JPADrawYBillboard(JPAEmitterWorkData*, JPABaseParticle*) {
}

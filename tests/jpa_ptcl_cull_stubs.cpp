// Stubs for tests/jpa_ptcl_cull.cpp (tools/test_jpa_ptcl_cull.sh): GX as a recorder of what a particle quad's picture depends on, and the parts
// of JPA the check never reaches. Plain C++ (the GX functions with C linkage and the argument types of the game's declarations).
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
#include <string.h>
struct Regs {
    float pos0[12], tex0[12];
    unsigned tev[4];
    const unsigned char* posArray;
};
struct C4 {
    uint8_t r, g, b, a;
};
Regs gRegs;
unsigned long long gGxCalls[3];  // GXSetTevColor, GXLoadPosMtxImm + GXLoadTexMtxImm, GXCallDisplayList
float gProj[7], gVp[6];
unsigned gScis[4];
void jpa_cull_record(const Regs* r, const unsigned char* list, unsigned nbytes);  // (tests/jpa_ptcl_cull.cpp)
void GXLoadPosMtxImm(const float mtx[3][4], unsigned id) {
    ++gGxCalls[1];
    if (id == 0) memcpy(gRegs.pos0, mtx, sizeof gRegs.pos0);
}
void GXLoadTexMtxImm(const float mtx[][4], unsigned id, int type) {
    ++gGxCalls[1];
    (void)type;
    if (id == 30) memcpy(gRegs.tex0, mtx, sizeof gRegs.tex0);  // (GX_TEXMTX0)
}
void GXSetTevColor(int id, C4 c) {
    ++gGxCalls[0];
    unsigned v;
    memcpy(&v, &c, 4);
    gRegs.tev[id & 3] = v;
}
void GXSetArray(int attr, const void* base, unsigned char stride) {
    (void)stride;
    if (attr == 9) gRegs.posArray = static_cast<const unsigned char*>(base);  // (GX_VA_POS)
}
void GXCallDisplayList(const void* list, unsigned nbytes) {
    ++gGxCalls[2];
    jpa_cull_record(&gRegs, static_cast<const unsigned char*>(list), nbytes);
}
void GXGetProjectionv(float* p) {
    memcpy(p, gProj, sizeof gProj);
}
void GXGetViewportv(float* v) {
    memcpy(v, gVp, sizeof gVp);
}
void GXGetScissor(unsigned* l, unsigned* t, unsigned* w, unsigned* h) {
    *l = gScis[0];
    *t = gScis[1];
    *w = gScis[2];
    *h = gScis[3];
}
#define GX_STUB(name) \
    void name() {     \
    }
GX_STUB(GXBegin)
GX_STUB(GXSetAlphaCompare)
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

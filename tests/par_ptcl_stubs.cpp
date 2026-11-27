// More GX stubs for tests/par_ptcl.cpp (JPAEmitterManager::draw is linked in, never called). Plain C++.
extern "C" {
#define GX_STUB(name) \
    void name() {     \
    }
GX_STUB(GXEnableTexOffsets)
GX_STUB(GXSetCullMode)
GX_STUB(GXSetCoPlanar)
GX_STUB(GXClearVtxDesc)
GX_STUB(GXSetVtxAttrFmt)
GX_STUB(GXSetCurrentMtx)
GX_STUB(GXSetChanCtrl)
GX_STUB(GXSetNumChans)
GX_STUB(port_gx_marker)
GX_STUB(__PortGXDrawSerial)
extern void* const __GXData; void* const __GXData = nullptr;  // (par25: referenced by JPAEmitterManager::draw, linked in and never called)
}

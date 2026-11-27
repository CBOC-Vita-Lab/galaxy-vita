/* tests31: the GX capture layer (platform/src/gx/gx_capture.cpp) is idle in this test: __PortGXCap stays 0, so the library's hooks never call it.
   These definitions only satisfy the linker (the stream census hook too). Used by tools/test_bloom_skip.sh. */
#include <stdint.h>
int __PortGXCap = 0, __PortGXCapPass = 0;
uintptr_t __PortGXCapReplayTop = 0, __PortGXCapReplaySpan = 0, __PortGXCapExTop = 0, __PortGXCapExSpan = 0;
#define STUB(n) void n(void) {}
STUB(port_gxcap_GXBegin) STUB(port_gxcap_GXCallDisplayList) STUB(port_gxcap_GXCopyDisp) STUB(port_gxcap_GXCopyTex) STUB(port_gxcap_GXGetScissor)
STUB(port_gxcap_GXGetViewportv) STUB(port_gxcap_GXLoadPosMtxImm) STUB(port_gxcap_GXSetCopyClear) STUB(port_gxcap_GXSetCopyFilter)
STUB(port_gxcap_GXSetCurrentMtx) STUB(port_gxcap_GXSetScissor) STUB(port_gxcap_GXSetTexCopyDst) STUB(port_gxcap_GXSetTexCopySrc)
STUB(port_gxcap_GXSetViewport) STUB(port_gxcap_GXSetViewportJitter) STUB(port_gxcap_port_gx_call_retained) STUB(port_gxcap_port_gx_draw_serial)
STUB(port_stream_census_copy)

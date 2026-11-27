/* GXV_GX_DEDUP end to end (tools/test_gx_dedup.sh): the GX capture layer's symbols the GX units name (its hooks; the layer stays off:
 * __PortGXCap is 0, so none of these runs) and the two PowerPC calls GXMisc.c makes. Old-style definitions: only the names matter. */
#include <stdint.h>
#include <stdlib.h>
int __PortGXCap = 0, __PortGXCapTrap = 0, __PortGXCapPass = 0, __PortGXCapFifo = 0;
uintptr_t __PortGXCapReplayTop = 0, __PortGXCapReplaySpan = 0, __PortGXCapExTop = 0, __PortGXCapExSpan = 0;
void port_gx_capture_init() {}
int port_gxcap_fifo_pre() { abort(); }
void port_gxcap_fifo_post() { abort(); }
int port_gxcap_stream_override() { return 0; }
unsigned PPCMfwpar() { return 0; }
void PPCSync() {}
void port_gxcap_GXBegin() { abort(); }
void port_gxcap_GXCallDisplayList() { abort(); }
void port_gxcap_GXClearVtxDesc() { abort(); }
void port_gxcap_GXFlush() { abort(); }
void port_gxcap_GXLoadPosMtxImm() { abort(); }
void port_gxcap_GXSetAlphaCompare() { abort(); }
void port_gxcap_GXSetArray() { abort(); }
void port_gxcap_GXSetBlendMode() { abort(); }
void port_gxcap_GXSetChanCtrl() { abort(); }
void port_gxcap_GXSetChanMatColor() { abort(); }
void port_gxcap_GXSetCoPlanar() { abort(); }
void port_gxcap_GXSetCurrentMtx() { abort(); }
void port_gxcap_GXSetNumChans() { abort(); }
void port_gxcap_GXSetNumTevStages() { abort(); }
void port_gxcap_GXSetNumTexGens() { abort(); }
void port_gxcap_GXSetTevAlphaIn() { abort(); }
void port_gxcap_GXSetTevAlphaOp() { abort(); }
void port_gxcap_GXSetTevColor() { abort(); }
void port_gxcap_GXSetTevColorIn() { abort(); }
void port_gxcap_GXSetTevColorOp() { abort(); }
void port_gxcap_GXSetTevKColor() { abort(); }
void port_gxcap_GXSetTevKColorSel() { abort(); }
void port_gxcap_GXSetTevOrder() { abort(); }
void port_gxcap_GXSetTexCoordGen2() { abort(); }
void port_gxcap_GXSetVtxAttrFmt() { abort(); }
void port_gxcap_GXSetVtxDesc() { abort(); }
void port_gxcap_GXSetZMode() { abort(); }
void port_gxcap_abandon_entry() { abort(); }

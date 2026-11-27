// GXV_ARRAYSKIP (lighter25, 11 Nov; default 0; exact): J3DShape::loadVtxArray writes the position / normal / colour array bases (CP 0xA0-0xA2)
// for every shape it draws. In a DrawBuffer drawer's run (the instances of one model, one material) they are the same addresses again and
// again. With the switch on, a base the stream already holds is not written (decomp J3DShape.cpp J3DLoadArrayBasePtr). The shadow of what
// the stream holds (__PortArrShadow, valid bits in __PortArrValid) is forgotten by everything else that may write those registers in the
// stream: every display-list call (GXCallDisplayList: the vertex-format lists write them) except the lists J3D makes without any CP command
// (material and differed lists, J3DDisplayListObj::callDL; shape geometry, J3DShapeDraw::draw: __PortArrKeep is set around those), GXSetArray,
// every port marker (a warm-up scope puts the front end's registers back), each display copy (a new picture), a GX capture; a write into a
// display list being recorded neither uses nor updates it.
//   1  on;  2  verify: nothing is left out, the writes that 1 would leave out are sent behind markers (BP 0xFF kinds 24-27: register and value)
//      that the front end checks against its register (vgx_state.cpp: the "arrayskip verify:" line must say 0 differing).
// Game thread (the thread that writes the FIFO). Counters: the "arrayskip:" line (platform/src/port/perf.cpp, every 300 pictures).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t __PortArrValid;      // bit n: __PortArrShadow[n] is what CP 0xA0 + n holds in the stream
extern uint32_t __PortArrShadow[3];
extern int __PortArrKeep;            // nonzero: the display list being called writes no CP register (J3D's own lists)
int port_arrayskip_mode(void);       // GXV_ARRAYSKIP (one load)
void port_arrayskip_note(int skipped);
void port_arrayskip_verify_mark(uint32_t reg, uint32_t value);  // mode 2: the markers before a write 1 would leave out
#ifdef __cplusplus
}
#endif

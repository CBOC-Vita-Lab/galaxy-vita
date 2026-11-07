#pragma once

#include <revolution/gd.h>
#include <revolution/gx/GXVert.h>

#ifdef TARGET_PC
// The same bytes as one batched command each (GXV_FIFO_BATCH, port/gx_fifo.h): the indexed matrix loads and CP array
// bases are the most frequent commands of the draw pass (a few per shape packet).
inline void J3DFifoLoadXFCmdHdr(u16 addr, u8 len) {
    __PortGXFifo_op_u16_u16(0x10, (u16)(len - 1), addr);
}

inline void J3DFifoLoadCPCmd(u8 reg, u32 value) {
    __PortGXFifo_op_u8_u32(0x08, reg, value);
}

inline void J3DFifoLoadBPCmd(u32 regval) {  // (two elements: no gain from a batch)
    GXCmd1u8(0x61);
    GXCmd1u32(regval);
}

inline void J3DFifoWriteXFCmdHdr(u16 addr, u8 len) {
    __PortGXFifo_op_u16_u16(0x10, (u16)(len - 1), addr);
}

inline void J3DFifoLoadIndx(u8 cmd, u16 indx, u16 addr) {
    __PortGXFifo_op_u16_u16(cmd, indx, addr);
}

inline void J3DFifoLoadNrmMtxIndx3x3(u16 index, u32 addr) {
    __PortGXFifo_op_u16_u16(0x28, index, (u16)(((9 - 1) << 12) | (u16)((addr * 3) + 0x400)));
}
#else
inline void J3DFifoLoadXFCmdHdr(u16 addr, u8 len) {
    GXCmd1u8(0x10);
    GXCmd1u16(len - 1);
    GXCmd1u16(addr);
}

inline void J3DFifoLoadCPCmd(u8 reg, u32 value) {
    GXCmd1u8(0x08);
    GXCmd1u8(reg);
    GXCmd1u32(value);
}

inline void J3DFifoLoadBPCmd(u32 regval) {
    GXCmd1u8(0x61);
    GXCmd1u32(regval);
}

inline void J3DFifoWriteXFCmdHdr(u16 addr, u8 len) {
    GXCmd1u8(0x10);
    GXCmd1u16(len - 1);
    GXCmd1u16(addr);
}

inline void J3DFifoLoadIndx(u8 cmd, u16 indx, u16 addr) {
    GXCmd1u8(cmd);
    GXCmd1u16(indx);
    GXCmd1u16(addr);
}

inline void J3DFifoLoadNrmMtxIndx3x3(u16 index, u32 addr) {
    GXCmd1u8(0x28);
    GXCmd1u16(index);
    GXCmd1u16(((9 - 1) << 12) | (u16)((addr * 3) + 0x400));
}

#endif

void J3DFifoLoadPosMtxImm(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxImm(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxImm3x3(f32 (*)[3], u32);
void J3DFifoLoadNrmMtxToTexMtx(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxToTexMtx3x3(f32 (*)[3], u32);
void J3DFifoLoadTexCached(GXTexMapID, u32, GXTexCacheSize, u32, GXTexCacheSize);

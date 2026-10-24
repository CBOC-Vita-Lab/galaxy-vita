import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

R = 'libs/RVL_SDK/include/revolution/gx/GXRegs.h'

sub(R, '''#ifdef __MWERKS__
extern volatile PPCWGPipe gxfifo : 0xCC008000;
#else
extern volatile PPCWGPipe gxfifo;
#endif''', '''#ifdef __MWERKS__
extern volatile PPCWGPipe gxfifo : 0xCC008000;
#else
#include "port/gx_fifo.h"
#endif''')

sub(R, '''/* GX fifo write helpers */

#define GX_WRITE_U8(ub) gxfifo.u8 = (u8)(ub)

#define GX_WRITE_U16(us) gxfifo.u16 = (u16)(us)

#define GX_WRITE_S16(us) gxfifo.s16 = (u16)(us)

#define GX_WRITE_U32(ui) gxfifo.u32 = (u32)(ui)

#define GX_WRITE_F32(f) gxfifo.f32 = (f32)(f);''', '''/* GX fifo write helpers */

#ifdef __MWERKS__
#define GX_WRITE_U8(ub) gxfifo.u8 = (u8)(ub)

#define GX_WRITE_U16(us) gxfifo.u16 = (u16)(us)

#define GX_WRITE_S16(us) gxfifo.s16 = (u16)(us)

#define GX_WRITE_U32(ui) gxfifo.u32 = (u32)(ui)

#define GX_WRITE_F32(f) gxfifo.f32 = (f32)(f);
#else
#define GX_WRITE_U8(ub) __PortGXFifo_u8((u8)(ub))

#define GX_WRITE_U16(us) __PortGXFifo_u16((u16)(us))

#define GX_WRITE_S16(us) __PortGXFifo_u16((u16)(us))

#define GX_WRITE_U32(ui) __PortGXFifo_u32((u32)(ui))

#define GX_WRITE_F32(f) __PortGXFifo_f32((f32)(f))
#endif''')

sub(R, '''#else
#define GX_CP_COUNTER_READ_U32(name)
#define GX_PE_COUNTER_READ_U32(name)
#define GX_MEM_COUNTER_READ_U32(name)
#define FAST_FLAG_SET(regOrg, newFlag, shift, size)
#endif''', '''#else
/* Performance/request counters of the real GPU; the port reports them as idle. */
#define GX_CP_COUNTER_READ_U32(name) 0u
#define GX_PE_COUNTER_READ_U32(name) 0u
#define GX_MEM_COUNTER_READ_U32(name) 0u

#define FAST_FLAG_SET(regOrg, newFlag, shift, size)                                                                                                  \\
    do {                                                                                                                                             \\
        (regOrg) = (u32)__rlwimi((u32)(regOrg), (u32)(newFlag), (shift), (32 - (shift) - (size)), (31 - (shift)));                                   \\
    } while (0);
#endif''')

V = 'libs/RVL_SDK/include/revolution/gx/GXVert.h'
sub(V, '''#ifdef __MWERKS__
volatile PPCWGPipe GXWGFifo : 0xCC008000;
#else
volatile PPCWGPipe GXWGFifo;
#endif''', '''#ifdef __MWERKS__
volatile PPCWGPipe GXWGFifo : 0xCC008000;
#define __GXFIFO_WRITE(td, v) GXWGFifo.td = (td)(v)
#else
#include "port/gx_fifo.h"
#define __GXFIFO_WRITE(td, v) __PortGXFifo_##td((td)(v))
#endif''')
sub(V, 'GXWGFifo.td = (td)x;', '__GXFIFO_WRITE(td, x);', count=0)
sub(V, 'GXWGFifo.td = (td)y;', '__GXFIFO_WRITE(td, y);', count=0)
sub(V, 'GXWGFifo.td = (td)z;', '__GXFIFO_WRITE(td, z);', count=0)
sub(V, 'GXWGFifo.td = (td)w;', '__GXFIFO_WRITE(td, w);', count=0)

I = 'src/RVL_SDK/gx/GXInit.c'
sub(I, '''    __piReg = (void*)OSPhysicalToUncached(0x0c003000);
    __cpReg = (void*)OSPhysicalToUncached(0x0c000000);
    __peReg = (void*)OSPhysicalToUncached(0x0c001000);
    __memReg = (void*)OSPhysicalToUncached(0x0c004000);''', '''#ifdef __MWERKS__
    __piReg = (void*)OSPhysicalToUncached(0x0c003000);
    __cpReg = (void*)OSPhysicalToUncached(0x0c000000);
    __peReg = (void*)OSPhysicalToUncached(0x0c001000);
    __memReg = (void*)OSPhysicalToUncached(0x0c004000);
#else
    /* No memory-mapped GPU on the host: the port keeps shadow register files. */
    {
        extern volatile u32 __PortGXPIRegs[0x400], __PortGXCPRegs[0x400], __PortGXPERegs[0x400], __PortGXMEMRegs[0x400];
        __piReg = __PortGXPIRegs;
        __cpReg = __PortGXCPRegs;
        __peReg = __PortGXPERegs;
        __memReg = __PortGXMEMRegs;
    }
#endif''')
sub(I, '''    PPCMtwpar((u32)OSUncachedToPhysical((void*)0xCC008000));''', '''#ifdef __MWERKS__
    PPCMtwpar((u32)OSUncachedToPhysical((void*)0xCC008000));
#endif''')

L = 'src/RVL_SDK/gx/GXLight.c'
sub(L, '''#else
static inline void WriteLightObjPS(const GXLightObjInt* lt_obj, void* dest);
#endif''', '''#else
static inline void WriteLightObjPS(const GXLightObjInt* lt_obj, void* dest) {
    int i;
    (void)dest;
    GX_WRITE_U32(0);
    GX_WRITE_U32(0);
    GX_WRITE_U32(0);
    GX_WRITE_U32(lt_obj->Color);
    for (i = 0; i < 3; i++) GX_WRITE_F32(lt_obj->a[i]);
    for (i = 0; i < 3; i++) GX_WRITE_F32(lt_obj->k[i]);
    for (i = 0; i < 3; i++) GX_WRITE_F32(lt_obj->lpos[i]);
    for (i = 0; i < 3; i++) GX_WRITE_F32(lt_obj->ldir[i]);
}
#endif''')

M = 'src/RVL_SDK/gx/GXMisc.c'
sub(M, '''void GXPeekARGB(u16 x, u16 y, u32* color) {
    u32 addr = (u32) OSPhysicalToUncached(0x8000000);''', '''#ifndef __MWERKS__
u32 __PortGXPeekARGB(u16 x, u16 y);
u32 __PortGXPeekZ(u16 x, u16 y);

void GXPeekARGB(u16 x, u16 y, u32* color) {
    *color = __PortGXPeekARGB(x, y);
}

void GXPeekZ(u16 x, u16 y, u32* z) {
    *z = __PortGXPeekZ(x, y);
}
#else
void GXPeekARGB(u16 x, u16 y, u32* color) {
    u32 addr = (u32) OSPhysicalToUncached(0x8000000);''')
sub(M, '''    SC_PE_PI_EFB_ADDR_SET_TYPE(addr,  1);
    *z = *(u32*)addr;
}''', '''    SC_PE_PI_EFB_ADDR_SET_TYPE(addr,  1);
    *z = *(u32*)addr;
}
#endif''')

sub('libs/JSystem/include/JSystem/J3DGraphBase/J3DFifo.hpp', '''    GXWGFifo.u8 = cmd;
    GXWGFifo.u16 = indx;
    GXWGFifo.u16 = addr;''', '''    GXCmd1u8(cmd);
    GXCmd1u16(indx);
    GXCmd1u16(addr);''')

sub('src/Game/MapObj/SpinDriverPathDrawer.cpp', '''    GXWGFifo.f32 = rA1.x;
    GXWGFifo.f32 = rA1.y;
    GXWGFifo.f32 = rA1.z;
    GXWGFifo.f32 = a2;
    GXWGFifo.f32 = a3;''', '''    GXCmd1f32(rA1.x);
    GXCmd1f32(rA1.y);
    GXCmd1f32(rA1.z);
    GXCmd1f32(a2);
    GXCmd1f32(a3);''')

done()

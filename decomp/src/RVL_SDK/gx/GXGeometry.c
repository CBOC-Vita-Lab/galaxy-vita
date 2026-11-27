#include "revolution/gx/GXEnum.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#define PORT_GXCAP_HOOKS 1  // (GX capture: the hook in each entry point, port/gx_capture.h)
#include "port/gx_capture.h"

void __GXSetAmbMat(u32 amState) {
    if (amState & 0x100) {
        GX_WRITE_XF_REG(0x100A, gx->ambColor[0], 0);
    }

    if (amState & 0x200) {
        GX_WRITE_XF_REG(0x100B, gx->ambColor[1], 0);
    }

    if (amState & 0x400) {
        GX_WRITE_XF_REG(0x100C, gx->matColor[0], 0);
    }

    if (amState & 0x800) {
        GX_WRITE_XF_REG(0x100D, gx->matColor[1], 0);
    }
}

void __GXSetLightChan(u32 chState) {
    u32 d, i, chIndx = 0x100E;

    if (chState & 0x1000000) {
        d = ((gx->genMode & 0x70) >> 4);
        GX_WRITE_XF_REG(0x1009, d, 0);
    }

    d = (chState & 0xF000) >> 12;
    i = 0;

    while (d) {
        if (d & 0x1) {
            GX_WRITE_XF_REG(chIndx, gx->chanCtrl[i], 0);
        }

        i++;
        chIndx++;
        d >>= 1;
    }
}

void __GXSetTexGen(u32 tgState) {
    u32 d, i, tgIndx = 0x1040;
    u32 dtgIndx = 0x1050;

    if (tgState & 0x2000000) {
        d = gx->genMode & 0xF;
        GX_WRITE_XF_REG(0x103F, d, 0);
    }

    d = (tgState & 0xFF0000) >> 16;
    i = 0;
    while (d) {
        if (d & 0x1) {
            GX_WRITE_XF_REG(tgIndx, gx->texGenCtrl[i], 0);
            GX_WRITE_XF_REG(dtgIndx, gx->dualTexGenCtrl[i], 0);
        }
    
        dtgIndx++;
        tgIndx++;
        i++;
        d >>= 1;    
    }
}

void __GXSetDirtyState() {
    u32 dState = gx->dirtyState;

    if (dState & 0x00000001) {
        __GXSetSUTexRegs();
    }

    if (dState & 0x00000002) {
        __GXUpdateBPMask();
    }

    if (dState & 0x00000004) {
        __GXSetGenMode();
    }

    if (dState & 0x00000008) {
        __GXSetVCD();
    }

    if (dState & 0x00000010) {
        __GXSetVAT();
    }
        
    if (dState & (0x00000008|0x00000010)) {
        __GXCalculateVLim();
    }

    dState &= 0xFFFFFF00;

    if (dState) {
        u32 dStateLocal;

        dStateLocal = dState & 0x00000F00;

        if (dStateLocal) {
            __GXSetAmbMat(dStateLocal);
        }
        
        dStateLocal = dState & (0x01000000|0x0000F000);

        if (dStateLocal) {
            __GXSetLightChan(dStateLocal);
        }

        dStateLocal = dState & (0x02000000|0x00FF0000);

        if (dStateLocal) {
            __GXSetTexGen(dStateLocal);
        }

        dStateLocal = dState & 0x04000000;
        if (dStateLocal) {
            __GXSetMatrixIndex(GX_VA_PNMTXIDX);
            __GXSetMatrixIndex(GX_VA_TEX4MTXIDX);
        }

        dStateLocal = dState & 0x10000000;
        if (dStateLocal) {
            __GXSetViewport();
        }

        dStateLocal = dState & 0x8000000;
        if (dStateLocal) {
            __GXSetProjection();
        } 

        gx->bpSentNot = GX_TRUE;
    }

    gx->dirtyState = 0;
}

void __GXSendFlushPrim(void);

#if defined(TARGET_PC)
// (2.3 revisit23, PETARI_BLOOM_SKIP) A count of the GX commands that can write the EFB: GXBegin, display-list calls
// (GXCallDisplayList, its gathered and retained forms) and EFB copies (GXCopyTex / GXCopyDisp: a copy may clear). The bloom
// (decomp Game/Screen/BloomEffect.cpp) reads it to know that nothing was drawn between its black fill and its glow chain.
// Game thread only (the GX library's FIFO writer); a plain counter, no barrier.
__attribute__((weak)) u32 __PortGXDrawSerial = 0;  // (weak: GXDisplayList.c and GXFrameBuf.c define it too, for tests that link only some units)
u32 port_gx_draw_serial(void) {
    PORT_GXCAP_RET(port_gx_draw_serial);
    return __PortGXDrawSerial;
}
#endif

#if defined(TARGET_PC)
extern int __PortJ3nEmitOn;  // (GXDisplayList.c: GXV_J3DNATIVE's unbracketed-emitter census)
void __PortJ3nEmitNote(u32 kind, const void* ra, u32 amount);
#endif
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    PORT_GXCAP(GXBegin, (type, vtxfmt, nverts));
#if defined(TARGET_PC)
    ++__PortGXDrawSerial;
    if (__PortJ3nEmitOn) __PortJ3nEmitNote(1, __builtin_return_address(0), nverts);
#endif
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }

    GX_WRITE_U8((vtxfmt | type));
    GX_WRITE_U16(nverts);
}

#if defined(TARGET_PC)
// GXV_HOTPROF_NOFLUSHSTRIP (hotprof25, 11 Nov; default 0; the switch: platform/src/port/perf.cpp). The flush primitive below is a
// workaround for the console's transform unit (GXSetMisc(GX_MT_XF_FLUSH), asked for by the particle and shadow code): a triangle strip
// whose bytes are all zero, sent in front of a primitive or a list call when a BP register was written since the last one. The console's
// execution-count profile: 6.92 M of them in 18,618 pictures (372 a picture, 43 % of all the primitives the front end's DrawStreamPlan
// sees). The vgx front end drops each one: every vertex of it is the same point, so it covers no pixel (vgx_state.cpp DrawStreamPlan);
// but only after the stream parse, the vertex-format plan and the array sync. With the switch on the strip is not written while the FIFO
// feeds the vgx front end's stream (__PortGXRetainedOk: GXV_RETAINED / GXV_DIRECT_J3D on as on the console, the write pointer in the
// stream: not while a display list is recorded, not with another renderer); the library's own state is left as the strip leaves it.
// The front end's command buffers are the same with and without the strips (tools/test_vgx_hotprof_noflush.sh).
int __PortGXRetainedOk(void) __attribute__((weak));        // (platform/src/gx/gx_fifo.cpp; port/gx_fifo.h)
int port_hotprof_noflushstrip_on(void) __attribute__((weak));  // (platform/src/port/perf.cpp)
#endif

void __GXSendFlushPrim(void) {
    u32 i, numD;
#if defined(TARGET_PC)
    if (port_hotprof_noflushstrip_on && port_hotprof_noflushstrip_on() && __PortGXRetainedOk && __PortGXRetainedOk()) {
        gx->bpSentNot = GX_TRUE;
        return;
    }
#endif
    numD = (u32)(gx->vNum * gx->vLim);
    GX_WRITE_U8((GX_VTXFMT0 | GX_TRIANGLESTRIP));
    GX_WRITE_U16(gx->vNum);

    for (i = 0; i < numD; i += 4) {
        GX_WRITE_U32(0);
    }

    gx->bpSentNot = GX_TRUE;
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    PORT_GXCAP(GXSetLineWidth, (width, texOffsets));
    SC_SU_LPSIZE_SET_LSIZE(gx->lpSize, width);
    SC_SU_LPSIZE_SET_LTOFF(gx->lpSize, texOffsets);
    GX_WRITE_RA_REG(gx->lpSize);
    gx->bpSentNot = GX_FALSE;
}

void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) {
    PORT_GXCAP(GXSetPointSize, (pointSize, texOffsets));
    SC_SU_LPSIZE_SET_PSIZE(gx->lpSize, pointSize);
    SC_SU_LPSIZE_SET_PTOFF(gx->lpSize, texOffsets);
    GX_WRITE_RA_REG(gx->lpSize);
    gx->bpSentNot = GX_FALSE;
}

void GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable) {
    PORT_GXCAP(GXEnableTexOffsets, (coord, line_enable, point_enable));
    SC_SU_TS0_SET_LF(gx->suTs0[coord], line_enable);
    SC_SU_TS0_SET_PF(gx->suTs0[coord], point_enable);
    GX_WRITE_RA_REG(gx->suTs0[coord]);
    gx->bpSentNot = GX_FALSE;
}

void GXSetCullMode(GXCullMode mode) {
    PORT_GXCAP(GXSetCullMode, (mode));
    GXCullMode hwMode = (GXCullMode)(((mode & 0x1)<<1) | ((mode & 0x2)>>1));
    SC_GEN_MODE_SET_REJECT_EN(gx->genMode, hwMode);
    gx->dirtyState |= 4;
}

void GXSetCoPlanar(GXBool enable) {
    PORT_GXCAP(GXSetCoPlanar, (enable));
    u32 reg;
    SC_GEN_MODE_SET_ZFREEZE(gx->genMode, enable);
    reg = SU_SSMASK(0x80000, 0xFE);
    GX_WRITE_RA_REG(reg);
    GX_WRITE_RA_REG(gx->genMode);
}

void __GXSetGenMode(void) {
    GX_WRITE_RA_REG(gx->genMode);
    gx->bpSentNot = GX_FALSE;
}

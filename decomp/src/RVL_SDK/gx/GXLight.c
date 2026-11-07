#include <math.h>
#include "private/xf_mem.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/gx/shortcut_xf_mem.h"

#define PI 3.14159265358979323846f
#define BIG_NUMBER 1.0E+18f

#if defined(TARGET_PC)
#include <string.h>
// ============================================================== (1.3 port) GXV_LIGHT_DEDUP (default 0; switch and scope:
// decomp/src/Game/Map/LightFunction.cpp port_light_scope_enter / _exit, used by DrawBufferGroup::drawOpa / drawXlu)
// Every shape drawer of a DrawBuffer pass loads the light of its first packet (DrawBufferShapeDrawer::draw -> ActorLightCtrl::loadLight
// -> LightFunction::loadActorLightInfo: lights 0, 1 and 2), so a model with ten materials loads the same three lights ten times
// in a row, and the next model of the pass, lit by the same area light, loads them again: ~3 x 69 FIFO bytes that the front end
// decodes word by word (WriteXFWord) only to find every word unchanged. Inside such a pass (a scope the game opens), a load whose
// sixteen words equal what this slot was last loaded with is not sent: the light memory already holds them.
//   Why the shadow is the light memory's content inside the scope: in this game XF light memory (0x600..0x67F) is written only by
// GXLoadLightObjImm (no GXLoadLightObjIndx exists; the J3D material lists' light objects, J3DColorBlockLightOn::mLight, are never
// set, so no J3D list writes lights; ShapePacketUserData's lists write texture-coordinate state only). The shadow is dropped when
// the scope opens and closes, and at every list call once a light load was ever recorded into a display list (gx->inDispList:
// such a list writes light memory when called; none is made in play). A load made while recording a list is never skipped.
// GX state other than the FIFO is as before: bpSentNot is set as the load sets it.
int __PortGXLightScope = 0;     // > 0: inside a pass the game opened (port_light_scope_enter)
u32 __PortGXLightGen = 1;       // moves whenever light memory may have changed (a load sent, the shadow dropped)
u32 __PortGXLightSent = 0;      // (counters for the "light-dedup:" line)
u32 __PortGXLightSkipped = 0;
static u32 sLightShadow[8][13];  // Color, a[3], k[3], lpos[3], ldir[3] as last sent to each slot (the three leading words are 0)
static u32 sLightShadowValid = 0;
static int sLightInList = 0;

void __PortGXLightShadowDrop(void) {
    sLightShadowValid = 0;
    ++__PortGXLightGen;
}

// GXCallDisplayList: a list that may hold a light load (see above).
void __PortGXLightListCall(void) {
    if (sLightInList) {
        __PortGXLightShadowDrop();
    }
}

// 1: the load is already in light memory (not sent); 0: send it (the shadow then holds it).
static int PortGXLightSame(const GXLightObjInt* obj, u32 idx) {
    u32 w[13];
    if (gx->inDispList) {
        sLightInList = 1;  // (into a list: light memory is written when the list is called)
        __PortGXLightShadowDrop();
        return 0;
    }
    if (__PortGXLightScope <= 0) {
        if (sLightShadowValid) {
            __PortGXLightShadowDrop();
        } else {
            ++__PortGXLightGen;
        }
        return 0;
    }
    w[0] = obj->Color;
    memcpy(&w[1], obj->a, 12);
    memcpy(&w[4], obj->k, 12);
    memcpy(&w[7], obj->lpos, 12);
    memcpy(&w[10], obj->ldir, 12);
    if ((sLightShadowValid & (1u << idx)) != 0 && memcmp(sLightShadow[idx], w, sizeof(w)) == 0) {
        ++__PortGXLightSkipped;
        return 1;
    }
    memcpy(sLightShadow[idx], w, sizeof(w));
    sLightShadowValid |= 1u << idx;
    ++__PortGXLightGen;
    ++__PortGXLightSent;
    return 0;
}
#endif

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    f32 r, d, cr, a0, a1, a2;
    GX_SETUP_LIGHT(obj, lt_obj)

    if (cutoff <= 0.0f || cutoff > 90.0f) {
        spot_func = GX_SP_OFF;
    }

    r = cutoff * PI / 180.0f;
    cr = cos(r);

    switch (spot_func) {
    case GX_SP_FLAT:
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case GX_SP_COS:
        a1 = 1.0f / (1.0f - cr);
        a0 = -cr * a1;
        a2 = 0.0f;
        break;
    case GX_SP_COS2:
        a2 = 1.0f / (1.0f - cr);
        a0 = 0.0F;
        a1 = -cr * a2;
        break;
    case GX_SP_SHARP:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a0 = cr * (cr - 2.0f) * d;
        a1 = 2.0f * d;
        a2 = -d;
        break;
    case GX_SP_RING1:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a2 = -4.0f * d;
        a0 = a2 * cr;
        a1 = 4.0f * (1.0f + cr) * d;
        break;
    case GX_SP_RING2:
        d = 1.0f / ((1.0f - cr) * (1.0f - cr));
        a0 = 1.0f - 2.0f * cr * cr * d;
        a1 = 4.0f * cr * d;
        a2 = -2.0f * d;
        break;
    case GX_SP_OFF:
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }

    obj->a[0] = a0;
    obj->a[1] = a1;
    obj->a[2] = a2;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    f32 k0, k1, k2;
    GX_SETUP_LIGHT(obj, lt_obj)

    if (ref_dist < 0.0f) {
        dist_func = GX_DA_OFF;
    }

    if (ref_br <= 0.0f || ref_br >= 1.0f) {
        dist_func = GX_DA_OFF;
    }

    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f;
        k1 = (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist);
        k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist);
        break;
    case GX_DA_OFF:
    default:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = 0.0f;
        break;
    }

    obj->k[0] = k0;
    obj->k[1] = k1;
    obj->k[2] = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->lpos[0] = x;
    obj->lpos[1] = y;
    obj->lpos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->ldir[0] = -nx;
    obj->ldir[1] = -ny;
    obj->ldir[2] = -nz;
}

void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    f32 mag;
    f32 vx, vy, vz;
    GX_SETUP_LIGHT(obj, lt_obj)
    vx = -nx;
    vy = -ny;
    vz = (-nz + 1.0f);
    mag = vx * vx + vy * vy + vz * vz;

    if (mag != 0.0f) {
        mag = 1.0f / (f32)sqrt(mag);
    }

    obj->ldir[0] = vx * mag;
    obj->ldir[1] = vy * mag;
    obj->ldir[2] = vz * mag;
    obj->lpos[0] = nx * -BIG_NUMBER;
    obj->lpos[1] = ny * -BIG_NUMBER;
    obj->lpos[2] = nz * -BIG_NUMBER;
}

void GXInitSpecularDirHA(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz, f32 hx, f32 hy, f32 hz) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->ldir[0] = hx;
    obj->ldir[1] = hy;
    obj->ldir[2] = hz;
    obj->lpos[0] = nx * -BIG_NUMBER;
    obj->lpos[1] = ny * -BIG_NUMBER;
    obj->lpos[2] = nz * -BIG_NUMBER;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color) {
    GX_SETUP_LIGHT(obj, lt_obj)
    obj->Color = GX_BE_U32(&color);
}

static inline u32 ConvLightID2Num(GXLightID id) {
    u32 nlz = (u32)__cntlzw(id);
    return (u32)(31 - nlz);
}

#ifdef __MWERKS__
#pragma optimizewithasm off
static inline void WriteLightObjPS(const register GXLightObjInt* lt_obj, register void* dest) {
    register u32 zero, color;
    register f32 a0_a1, a2_k0, k1_k2;
    register f32 px_py, pz_dx, dy_dz;

    asm
    {
        lwz     color, 12(lt_obj)
        xor     zero, zero, zero
        psq_l   a0_a1, 16(lt_obj), 0, 0
        psq_l   a2_k0, 24(lt_obj), 0, 0
        psq_l   k1_k2, 32(lt_obj), 0, 0
        psq_l   px_py, 40(lt_obj), 0, 0
        psq_l   pz_dx, 48(lt_obj), 0, 0
        psq_l   dy_dz, 56(lt_obj), 0, 0
        
        stw     zero,  0(dest)
        stw     zero,  0(dest)
        stw     zero,  0(dest)
        stw     color, 0(dest)
        psq_st  a0_a1, 0(dest), 0, 0
        psq_st  a2_k0, 0(dest), 0, 0
        psq_st  k1_k2, 0(dest), 0, 0
        psq_st  px_py, 0(dest), 0, 0
        psq_st  pz_dx, 0(dest), 0, 0
        psq_st  dy_dz, 0(dest), 0, 0
    }
}
#else
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
#endif

void GXLoadLightObjImm(const GXLightObj* lt_obj, GXLightID light) {
    u32 addr, idx;
    GX_SETUP_LIGHT(obj, lt_obj)
    idx = ConvLightID2Num(light);
    idx &= 7;
    addr = (0x600 + idx * 0x10);
#if defined(TARGET_PC)
    if (PortGXLightSame(obj, idx)) {
        gx->bpSentNot = GX_TRUE;  // (GXV_LIGHT_DEDUP: these words are in light memory already; GX state as after the load)
        return;
    }
    {
        // GXV_FIFO_BATCH (port/gx_fifo.h): the header and the light's sixteen words as one batched command (the same 69 bytes
        // as WriteLightObjPS's element-wise writes, which run when the batch is off or would reach the FIFO's limit).
        u8* p;
        if (__PortGXFifoRoom(&p, 5 + 64)) {
            int i;
            p = __PortGXPut_u8(p, CP_OPCODE(0, 2));
            p = __PortGXPut_u32(p, CP_XF_LOADREGS((addr), 0xF));
            p = __PortGXPut_u32(p, 0);
            p = __PortGXPut_u32(p, 0);
            p = __PortGXPut_u32(p, 0);
            p = __PortGXPut_u32(p, obj->Color);
            for (i = 0; i < 3; i++) p = __PortGXPut_f32(p, obj->a[i]);
            for (i = 0; i < 3; i++) p = __PortGXPut_f32(p, obj->k[i]);
            for (i = 0; i < 3; i++) p = __PortGXPut_f32(p, obj->lpos[i]);
            for (i = 0; i < 3; i++) p = __PortGXPut_f32(p, obj->ldir[i]);
            __PortGXFifoCommit(p);
            gx->bpSentNot = GX_TRUE;
            return;
        }
    }
#endif
    GX_WRITE_U8(CP_OPCODE(0, 2));
    GX_WRITE_U32(CP_XF_LOADREGS((addr), 0xF));
    WriteLightObjPS(obj, (void*)0xCC008000);
    gx->bpSentNot = GX_TRUE;
}

#pragma optimizewithasm reset

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    u32 reg = 0, rgb, colIdx;
    switch (chan) {
    case GX_COLOR0:
        reg = gx->ambColor[GX_COLOR0];
        rgb = ((GX_BE_U32(&amb_color)) >> 8);
        SC_XF_AMBIENT0_F_SET_RGB(reg, rgb);
        colIdx = 0;
        break;
    case GX_COLOR1:
        reg = gx->ambColor[GX_COLOR1];
        rgb = ((GX_BE_U32(&amb_color)) >> 8);
        SC_XF_AMBIENT1_F_SET_RGB(reg, rgb);
        colIdx = 1;
        break;
    case GX_ALPHA0:
        reg = gx->ambColor[GX_COLOR0];
        SC_XF_AMBIENT0_F_SET_ALPHA(reg, amb_color.a);
        colIdx = 0;
        break;
    case GX_ALPHA1:
        reg = gx->ambColor[GX_COLOR1];
        SC_XF_AMBIENT1_F_SET_ALPHA(reg, amb_color.a);
        colIdx = 1;
        break;
    case GX_COLOR0A0:
        SC_XF_AMBIENT0_F_SET_RGBA(reg, GX_BE_U32(&amb_color));
        colIdx = 0;
        break;
    case GX_COLOR1A1:
        SC_XF_AMBIENT1_F_SET_RGBA(reg, GX_BE_U32(&amb_color));
        colIdx = 1;
        break;
    default:
        return;
    }

    gx->dirtyState |= (0x100 << colIdx);
    gx->ambColor[colIdx] = reg;
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    u32 reg = 0, rgb, colIdx;

    switch (chan) {
    case GX_COLOR0:
        reg = gx->matColor[GX_COLOR0];
        rgb = ((GX_BE_U32(&mat_color)) >> 8);
        SC_XF_MATERIAL0_F_SET_RGB(reg, rgb);
        colIdx = 0;
        break;

    case GX_COLOR1:
        reg = gx->matColor[GX_COLOR1];
        rgb = ((GX_BE_U32(&mat_color)) >> 8);
        SC_XF_MATERIAL1_F_SET_RGB(reg, rgb);
        colIdx = 1;
        break;

    case GX_ALPHA0:
        reg = gx->matColor[GX_COLOR0];
        SC_XF_MATERIAL0_F_SET_ALPHA(reg, mat_color.a);
        colIdx = 0;
        break;

    case GX_ALPHA1:
        reg = gx->matColor[GX_COLOR1];
        SC_XF_MATERIAL1_F_SET_ALPHA(reg, mat_color.a);
        colIdx = 1;
        break;

    case GX_COLOR0A0:
        SC_XF_MATERIAL0_F_SET_RGBA(reg, GX_BE_U32(&mat_color));
        colIdx = 0;
        break;

    case GX_COLOR1A1:
        SC_XF_MATERIAL1_F_SET_RGBA(reg, GX_BE_U32(&mat_color));
        colIdx = 1;
        break;

    default:
        return;
    }

    gx->dirtyState |= (0x400 << colIdx);
    gx->matColor[colIdx] = reg;
}

void GXSetNumChans(u8 nChans) {
    SC_GEN_MODE_SET_NCOL(gx->genMode, nChans);
    gx->dirtyState |= 0x1000004;
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    u32 reg, idx;
    idx = (u32)(chan & 0x0003);

    reg = 0;
    SC_XF_COLOR0CNTRL_F_SET_LIGHTFUNC(reg, enable);
    SC_XF_COLOR0CNTRL_F_SET_MATERIAL_SRC(reg, mat_src);
    SC_XF_COLOR0CNTRL_F_SET_AMBIENT_SRC(reg, amb_src);
    SC_XF_COLOR0CNTRL_F_SET_DIFFUSEATTEN(reg, ((attn_fn == GX_AF_SPEC) ? GX_DF_NONE : diff_fn));
    SC_XF_COLOR0CNTRL_F_SET_ATTENENABLE(reg, (attn_fn != GX_AF_NONE));
    SC_XF_COLOR0CNTRL_F_SET_ATTENSELECT(reg, (attn_fn != GX_AF_SPEC));
    SC_XF_COLOR0CNTRL_F_SET_LIGHT0123(reg, (light_mask & 0x000F));
    SC_XF_COLOR0CNTRL_F_SET_LIGHT4567(reg, ((light_mask & 0x00F0) >> 4));

    gx->chanCtrl[idx] = reg;
    gx->dirtyState |= (0x1000 << idx);

    if (chan == GX_COLOR0A0) {
        gx->chanCtrl[GX_ALPHA0] = reg;
        gx->dirtyState |= (0x00001000 | 0x00004000);
    } else if (chan == GX_COLOR1A1) {
        gx->chanCtrl[GX_ALPHA1] = reg;
        gx->dirtyState |= (0x00002000 | 0x00008000);
    }
}

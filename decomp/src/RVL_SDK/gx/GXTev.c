#include "revolution/gx/GXTev.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/gx.h"
#include <string.h>
#define PORT_GXCAP_HOOKS 1  // (GX capture: the hook in each entry point, port/gx_capture.h)
#include "port/gx_capture.h"
#include "port/compat.h"  // (PORT_GX_MARK_TEVCOL_REG / _VAL)

static tev_color_env_t TEVCOpTableST0[] = {
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_TEXC,
        GX_CC_RASC,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_RASC,
        GX_CC_TEXC,
        GX_CC_TEXA,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_RASC,
        GX_CC_ONE, 
        GX_CC_TEXC,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_TEXC  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_RASC  
    },
};

static tev_color_env_t TEVCOpTableST1[] =
{
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_TEXC,
        GX_CC_CPREV,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_CPREV,
        GX_CC_TEXC,
        GX_CC_TEXA,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_CPREV,
        GX_CC_ONE, 
        GX_CC_TEXC,
        GX_CC_ZERO  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_TEXC  
    },
    {
        0xC0,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_ZERO,
        GX_CC_CPREV 
    },
};

static tev_alpha_env_t TEVAOpTableST0[] =
{
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA,
        GX_CA_RASA,
        GX_CA_ZERO, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_RASA, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA,
        GX_CA_RASA,
        GX_CA_ZERO, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_RASA, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
};

static tev_alpha_env_t TEVAOpTableST1[] =
{
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA,
        GX_CA_APREV,
        GX_CA_ZERO, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_APREV,
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA,
        GX_CA_APREV,
        GX_CA_ZERO, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_TEXA, 
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
    {
        0xC1,
        GX_TEVPREV,
        GX_CS_SCALE_1,
        GX_TRUE,
        GX_TEV_ADD,
        GX_TB_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_ZERO,
        GX_CA_APREV,
        GX_TEV_SWAP0,
        GX_TEV_SWAP0,
    },
};

#ifdef TARGET_PC
/* The preset tables are bitfield structs that CodeWarrior allocates from the
 * most significant bit; build the register words explicitly. */
static u32 PackTevColorEnv(const tev_color_env_t* e) {
    return ((u32)e->rid << 24) | ((u32)e->dest << 22) | ((u32)e->shift << 20) | ((u32)e->clamp << 19) | ((u32)e->sub << 18) |
           ((u32)e->bias << 16) | ((u32)e->sela << 12) | ((u32)e->selb << 8) | ((u32)e->selc << 4) | (u32)e->seld;
}

static u32 PackTevAlphaEnv(const tev_alpha_env_t* e) {
    return ((u32)e->rid << 24) | ((u32)e->dest << 22) | ((u32)e->shift << 20) | ((u32)e->clamp << 19) | ((u32)e->sub << 18) |
           ((u32)e->bias << 16) | ((u32)e->sela << 13) | ((u32)e->selb << 10) | ((u32)e->selc << 7) | ((u32)e->seld << 4) |
           ((u32)e->swap << 2) | (u32)e->mode;
}
#endif

void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    PORT_GXCAP(GXSetTevOp, (id, mode));
    u32 *ctmp, *atmp, tevReg;
#ifdef TARGET_PC
    u32 cword, aword;
#endif

    if (id == GX_TEVSTAGE0) {
        ctmp = (u32*)&TEVCOpTableST0[mode];
        atmp = (u32*)&TEVAOpTableST0[mode];
    }
    else {
        ctmp = (u32*)&TEVCOpTableST1[mode];
        atmp = (u32*)&TEVAOpTableST1[mode];
    }
#ifdef TARGET_PC
    cword = PackTevColorEnv((const tev_color_env_t*)ctmp);
    aword = PackTevAlphaEnv((const tev_alpha_env_t*)atmp);
    ctmp = &cword;
    atmp = &aword;
#endif

    tevReg = gx->tevc[id];
    tevReg = ((*ctmp & ~0xFF000000) | (tevReg & 0xFF000000));
    GX_WRITE_RA_REG(tevReg);
    gx->tevc[id] = tevReg;

    tevReg = gx->teva[id];
    tevReg = ((*atmp & ~0xFF00000F) | (tevReg & 0xFF00000F));
    GX_WRITE_RA_REG(tevReg);
    gx->teva[id] = tevReg;
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevColorIn( GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d ) {
    PORT_GXCAP(GXSetTevColorIn, (stage, a, b, c, d));
    u32 tevReg = gx->tevc[stage];
    SC_TEV_COLOR_ENV_SET_SELA(tevReg, a);
    SC_TEV_COLOR_ENV_SET_SELB(tevReg, b);
    SC_TEV_COLOR_ENV_SET_SELC(tevReg, c);
    SC_TEV_COLOR_ENV_SET_SELD(tevReg, d);
    GX_WRITE_RA_REG(tevReg);
    gx->tevc[stage] = tevReg;
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    PORT_GXCAP(GXSetTevAlphaIn, (stage, a, b, c, d));
    u32 tevReg = gx->teva[stage];
    SC_TEV_ALPHA_ENV_SET_SELA(tevReg, a);
    SC_TEV_ALPHA_ENV_SET_SELB(tevReg, b);
    SC_TEV_ALPHA_ENV_SET_SELC(tevReg, c);
    SC_TEV_ALPHA_ENV_SET_SELD(tevReg, d);
    GX_WRITE_RA_REG(tevReg);
    gx->teva[stage] = tevReg;
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg ) {
    PORT_GXCAP(GXSetTevColorOp, (stage, op, bias, scale, clamp, out_reg));
    u32 tevReg = gx->tevc[stage];
    SC_TEV_COLOR_ENV_SET_SUB(tevReg, (op & 1));

    if (op <= GX_TEV_SUB) {
        SC_TEV_COLOR_ENV_SET_SHIFT(tevReg, scale);
        SC_TEV_COLOR_ENV_SET_BIAS(tevReg, bias);
    } else {
        SC_TEV_COLOR_ENV_SET_SHIFT(tevReg, ((op >> 1) & 3));
        SC_TEV_COLOR_ENV_SET_BIAS(tevReg, GX_MAX_TEVBIAS);
    }

    SC_TEV_COLOR_ENV_SET_CLAMP(tevReg, clamp);
    SC_TEV_COLOR_ENV_SET_DEST(tevReg, out_reg);
    GX_WRITE_RA_REG(tevReg);
    gx->tevc[stage] = tevReg;
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg) {
    PORT_GXCAP(GXSetTevAlphaOp, (stage, op, bias, scale, clamp, out_reg));
    u32 tevReg = gx->teva[stage];
    SC_TEV_ALPHA_ENV_SET_SUB(tevReg, (op & 1));
    if (op <= GX_TEV_SUB) {
        SC_TEV_ALPHA_ENV_SET_SHIFT(tevReg, scale);
        SC_TEV_ALPHA_ENV_SET_BIAS(tevReg, bias);
    } else {
        SC_TEV_ALPHA_ENV_SET_SHIFT(tevReg, ((op >> 1) & 3));
        SC_TEV_ALPHA_ENV_SET_BIAS(tevReg, GX_MAX_TEVBIAS);
    }

    SC_TEV_ALPHA_ENV_SET_CLAMP(tevReg, clamp);
    SC_TEV_ALPHA_ENV_SET_DEST(tevReg, out_reg);
    GX_WRITE_RA_REG(tevReg);
    gx->teva[stage] = tevReg;
    gx->bpSentNot = GX_FALSE;
}

#if defined(TARGET_PC)
// GXV_HOTPROF_TEVCOL (hotprof25 round 2, 11 Nov; default 0; the switch: platform/src/port/perf.cpp). From the console's execution-count
// profile: GXSetTevColor is called 15.5 M times in 18,618 pictures (832 a picture, two per particle quad) and writes four BP commands
// each: 42 % of all the BP commands of the stream.
//   (a) The console's library writes the BG word three times (a latch workaround of the hardware). For the vgx front end the second
//       and third are writes of the value the register was given by the command right before them: they change nothing, whatever
//       else the stream holds. With the switch on the BG word is written once while the FIFO feeds the vgx front end's stream
//       (__PortGXRetainedOk: not into a display list being recorded, not for another renderer). No state is kept for this.
//   (b) Inside a particle loop of JParticle (port_gx_tevcol_scope, opened by JPAResource::drawP / drawC for loops whose particle
//       functions are the known set: they call GXSetTevColor, GXLoadPosMtxImm, GXLoadTexMtxImm and GXCallDisplayList of the two quad
//       lists and nothing else) an RA or BG word equal to the one this function last wrote for that register IN THIS LOOP is not
//       written. Why the front end holds it: the stream is read in order; since that write, the only bytes put into the stream are
//       those four functions' (XF loads, a list call of a list without a BP command, the library's dirty state and flush strip, which
//       write no BP register 0xE0..0xE7), so the register still holds the word. The memory of what was written starts empty at
//       every loop (nothing from before the loop is trusted: a display list or a direct FIFO writer may have written the registers).
//       GX capture: nothing is left out by (b) while the capture layer records or replays (__PortGXCap, __PortGXCapPass): a recorded
//       call is replayed through this function, which then writes in full and remembers what it wrote; (a) keeps no state, so record
//       and replay agree.
//   The library's own state is as before in every case (gx->bpSentNot is cleared whether or not a word was left out).
//   2: verify: everything is written as before; each word that 1 would leave out is announced by two port markers (BP 0xFF kinds 28
//      and 29: the register and its value) that the front end checks against its register file ("tevcol verify:" line, 0 differing).
//   "tevcol:" line every 65,536 calls: calls, BP words written, repeats and equal halves left out (or announced).
int port_hotprof_tevcol_mode(void) __attribute__((weak));
int __PortGXRetainedOk(void) __attribute__((weak));          // (platform/src/gx/gx_fifo.cpp)
void port_log_summary(const char* fmt, ...) __attribute__((weak));
unsigned char __PortGXTevColScope = 0;
static u32 sTevColRA[4], sTevColBG[4];
static u8 sTevColValid[4];
static unsigned long long sTevColCalls, sTevColWords, sTevColRepeats, sTevColEqual;
void port_gx_tevcol_scope(int on) {
    sTevColValid[0] = sTevColValid[1] = sTevColValid[2] = sTevColValid[3] = 0;
    __PortGXTevColScope = (unsigned char)(on != 0);
}
void port_gx_tevcol_drop(void) {  // (GXV_PIPE25's firewall, GXDisplayList.c: what the stream holds is not known any more)
    sTevColValid[0] = sTevColValid[1] = sTevColValid[2] = sTevColValid[3] = 0;
}
static void PortTevColMark(u32 reg) {  // (mode 2: "the next write of this word must change nothing")
    GX_WRITE_RA_REG((0xFFu << 24) | (((reg >> 16) & 0xFFFFu) << 8) | PORT_GX_MARK_TEVCOL_REG);
    GX_WRITE_RA_REG((0xFFu << 24) | ((reg & 0xFFFFu) << 8) | PORT_GX_MARK_TEVCOL_VAL);
}
// 1: the words were written (or left out) here; 0: the caller writes them as the console's library does.
static int PortTevColSend(u32 id, u32 regRA, u32 regBG, int colour) {
    const int mode = port_hotprof_tevcol_mode ? port_hotprof_tevcol_mode() : 0;
    int dedup;
    u32 i = id & 3u;
    if (mode == 0 || !__PortGXRetainedOk || !__PortGXRetainedOk()) {
        if (colour) sTevColValid[i] = 0;  // (written elsewhere than into the stream, or by the plain path: nothing is known)
        return 0;
    }
    dedup = colour && __PortGXTevColScope && !__PortGXCap && !__PortGXCapPass;
    ++sTevColCalls;
    if (mode == 1) {
        if (dedup && (sTevColValid[i] & 1) && sTevColRA[i] == regRA) {
            ++sTevColEqual;
        } else {
            GX_WRITE_RA_REG(regRA);
            ++sTevColWords;
        }
        if (dedup && (sTevColValid[i] & 2) && sTevColBG[i] == regBG) {
            ++sTevColEqual;
        } else {
            GX_WRITE_RA_REG(regBG);
            ++sTevColWords;
        }
        sTevColRepeats += 2;
    } else {
        if (dedup && (sTevColValid[i] & 1) && sTevColRA[i] == regRA) {
            PortTevColMark(regRA);
            ++sTevColEqual;
        }
        GX_WRITE_RA_REG(regRA);
        if (dedup && (sTevColValid[i] & 2) && sTevColBG[i] == regBG) {
            PortTevColMark(regBG);
            ++sTevColEqual;
        }
        GX_WRITE_RA_REG(regBG);
        PortTevColMark(regBG);
        GX_WRITE_RA_REG(regBG);
        PortTevColMark(regBG);
        GX_WRITE_RA_REG(regBG);
        sTevColWords += 4;
        sTevColRepeats += 2;
    }
    if (colour) {
        sTevColRA[i] = regRA;
        sTevColBG[i] = regBG;
        sTevColValid[i] = 3;
    }
    if ((sTevColCalls & 0xFFFFu) == 0 && port_log_summary) {
        port_log_summary("tevcol: GXV_HOTPROF_TEVCOL=%d: since the start %llu TEV colour calls, %llu BP words written, %llu repeats of the BG word and "
                         "%llu words equal to the particle loop's last %s",
                         mode, sTevColCalls, sTevColWords, sTevColRepeats, sTevColEqual, mode == 1 ? "not written" : "announced to the front end (verify)");
    }
    return 1;
}
#endif

void GXSetTevColor(GXTevRegID id, GXColor color) {
    PORT_GXCAP(GXSetTevColor, (id, color));
    u32 rgba, regRA, regBG;
    rgba = GX_BE_U32(&color);
    regRA = TEV_REGISTERL(0, 0, (0xE0 + id * 2));
    SC_TEV_REGISTERL_SET_R8(regRA, (rgba >> 24) & 0xFF);
    SC_TEV_REGISTERL_SET_A8(regRA, rgba & 0xFF);

    regBG = TEV_REGISTERH(0, 0, (0xE1 + id * 2));
    SC_TEV_REGISTERH_SET_B8(regBG, (rgba >> 8) & 0xFF);
    SC_TEV_REGISTERH_SET_G8(regBG, (rgba >> 16) & 0xFF);

#if defined(TARGET_PC)
    if (__PortGXFx) {  // (GXV_FXMERGE: gathered with the particle loop's other calls, GXDisplayList.c)
        __PortGXFxTevColor(regRA, regBG);
        gx->bpSentNot = GX_FALSE;
        return;
    }
    if (PortTevColSend((u32)id, regRA, regBG, 1)) {  // (GXV_HOTPROF_TEVCOL, above)
        gx->bpSentNot = GX_FALSE;
        return;
    }
#endif
    GX_WRITE_RA_REG(regRA);
    GX_WRITE_RA_REG(regBG);
    GX_WRITE_RA_REG(regBG);
    GX_WRITE_RA_REG(regBG);
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    PORT_GXCAP(GXSetTevColorS10, (id, color));
    u32 sRG, sBA, regRA, regBG;
    sRG = ((u32)(u16)color.r << 16) | (u16)color.g;
    sBA = ((u32)(u16)color.b << 16) | (u16)color.a;
    regRA = TEV_REGISTERL(0, 0, (0xE0 + id * 2));
    SC_TEV_REGISTERL_SET_R(regRA, (sRG >> 16) & ((1 << TEV_REGISTERL_R_SIZE) - 1));
    SC_TEV_REGISTERL_SET_A(regRA, sBA & ((1 << TEV_REGISTERL_A_SIZE) - 1));

    regBG = TEV_REGISTERH(0, 0, (0xE1 + id * 2));
    SC_TEV_REGISTERH_SET_B(regBG, (sBA >> 16) & ((1 << TEV_REGISTERH_B_SIZE) - 1));
    SC_TEV_REGISTERH_SET_G(regBG, sRG & ((1 << TEV_REGISTERH_G_SIZE) - 1));

#if defined(TARGET_PC)
    // (GXV_HOTPROF_TEVCOL (a): the BG word once; this function is not one of a particle loop's, so (b) never applies, and what the
    // loop's memory holds for the register is forgotten)
    sTevColValid[(u32)id & 3u] = 0;
    if (PortTevColSend((u32)id, regRA, regBG, 0)) {
        gx->bpSentNot = GX_FALSE;
        return;
    }
#endif
    GX_WRITE_RA_REG(regRA);
    GX_WRITE_RA_REG(regBG);
    GX_WRITE_RA_REG(regBG);
    GX_WRITE_RA_REG(regBG);
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    PORT_GXCAP(GXSetTevKColor, (id, color));
    u32 rgba, regRA, regBG;
    rgba = GX_BE_U32(&color);
    regRA = TEV_KREGISTERL(0, 0, (0xE0 + id * 2));
    SC_TEV_KREGISTERL_SET_R(regRA, (rgba >> 24) & 0xFF);
    SC_TEV_KREGISTERL_SET_A(regRA, rgba & 0xFF);
    SC_TEV_KREGISTERL_SET_PAD1(regRA, 0x08);

    regBG = TEV_KREGISTERH(0, 0, (0xE1 + id * 2));
    SC_TEV_KREGISTERH_SET_B(regBG, (rgba >> 8) & 0xFF);
    SC_TEV_KREGISTERH_SET_G(regBG, (rgba >> 16) & 0xFF);
    SC_TEV_KREGISTERH_SET_PAD1(regBG, 0x08);

    GX_WRITE_RA_REG(regRA);
    GX_WRITE_RA_REG(regBG); 
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    PORT_GXCAP(GXSetTevKColorSel, (stage, sel));
    u32* reg = &gx->tevKsel[stage >> 1];
    if (stage & 1) {
        SC_TEV_KSEL_SET_KCSEL1((*reg), sel);
    }
    else {
        SC_TEV_KSEL_SET_KCSEL0((*reg), sel);
    }

    GX_WRITE_RA_REG((*reg));
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    PORT_GXCAP(GXSetTevKAlphaSel, (stage, sel));
    u32* reg = &gx->tevKsel[stage >> 1];
    if (stage & 1) {
        SC_TEV_KSEL_SET_KASEL1((*reg), sel);
    }
    else {
        SC_TEV_KSEL_SET_KASEL0((*reg), sel);
    }

    GX_WRITE_RA_REG((*reg));
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    PORT_GXCAP(GXSetTevSwapMode, (stage, ras_sel, tex_sel));
    u32* reg = &gx->teva[stage];
    SC_TEV_ALPHA_ENV_SET_MODE((*reg), ras_sel);
    SC_TEV_ALPHA_ENV_SET_SWAP((*reg), tex_sel);
    GX_WRITE_RA_REG((*reg));
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha) {
    PORT_GXCAP(GXSetTevSwapModeTable, (table, red, green, blue, alpha));
    u32* reg = &gx->tevKsel[table << 1];
    SC_TEV_KSEL_SET_XRB((*reg), red);
    SC_TEV_KSEL_SET_XGA((*reg), green);
    GX_WRITE_RA_REG((*reg));

    reg = &gx->tevKsel[(table << 1) + 1];
    SC_TEV_KSEL_SET_XRB((*reg), blue);
    SC_TEV_KSEL_SET_XGA((*reg), alpha);
    GX_WRITE_RA_REG((*reg));
    gx->bpSentNot = GX_FALSE;
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    PORT_GXCAP(GXSetAlphaCompare, (comp0, ref0, op, comp1, ref1));
    u32 reg = 0xF3000000;
    SC_TEV_ALPHAFUNC_SET_A0(reg, ref0);
    SC_TEV_ALPHAFUNC_SET_A1(reg, ref1);
    SC_TEV_ALPHAFUNC_SET_OP0(reg, comp0);
    SC_TEV_ALPHAFUNC_SET_OP1(reg, comp1);
    SC_TEV_ALPHAFUNC_SET_LOGIC(reg, op);
    GX_WRITE_RA_REG(reg);
    gx->bpSentNot = GX_FALSE;
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    PORT_GXCAP(GXSetZTexture, (op, fmt, bias));
    u32 zenv0, zenv1, type;
    zenv0 = 0;
    SC_TEV_Z_ENV_0_SET_ZOFF(zenv0, bias);
    SC_TEV_Z_ENV_0_SET_RID(zenv0, 0xF4);

    zenv1 = 0;
    switch (fmt) {
        case GX_TF_Z8:
            type = 0;
            break;
        case GX_TF_Z16:
            type = 1;
            break;
        case GX_TF_Z24X8:
            type = 2;
            break;
        default:
            type = 2;
            break;
    }

    SC_TEV_Z_ENV_1_SET_TYPE(zenv1, type);
    SC_TEV_Z_ENV_1_SET_OP(zenv1, op);
    SC_TEV_Z_ENV_1_SET_RID(zenv1, 0xF5);
   
    GX_WRITE_RA_REG(zenv0);
    GX_WRITE_RA_REG(zenv1);
    gx->bpSentNot = GX_FALSE;
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    PORT_GXCAP(GXSetTevOrder, (stage, coord, map, color));
    u32* ptref, tmap, tcoord;
    static u32 c2r[] = {0,1,0,1,0,1,7,5,6};
    ptref = &gx->tref[stage / 2];
    gx->texmapId[stage] = map;
    tmap = (u32)(map & ~GX_TEX_DISABLE);
    tmap = (tmap >= GX_MAX_TEXMAP) ? GX_TEXMAP0 : tmap;

    if (coord >= GX_MAX_TEXCOORD) {
        tcoord = GX_TEXCOORD0;
        gx->tevTcEnab = gx->tevTcEnab & ~(1 << stage);
    }
    else {
        tcoord = coord;
        gx->tevTcEnab = gx->tevTcEnab | (1 << stage);
    }

    if (stage & 1) {
        SC_RAS1_TREF_SET_TI1(*ptref, tmap);
        SC_RAS1_TREF_SET_TC1(*ptref, tcoord);
        SC_RAS1_TREF_SET_CC1(*ptref, ((color == GX_COLOR_NULL) ? 7 : c2r[color]));
        SC_RAS1_TREF_SET_TE1(*ptref, ((map != GX_TEXMAP_NULL) && !(map & GX_TEX_DISABLE)));
    }
    else {
        SC_RAS1_TREF_SET_TI0(*ptref, tmap);
        SC_RAS1_TREF_SET_TC0(*ptref, tcoord);
        SC_RAS1_TREF_SET_CC0(*ptref, ((color == GX_COLOR_NULL) ? 7 : c2r[color]));
        SC_RAS1_TREF_SET_TE0(*ptref, ((map != GX_TEXMAP_NULL) && !(map & GX_TEX_DISABLE)));
    }

    GX_WRITE_RA_REG(*ptref);
    gx->bpSentNot = GX_FALSE;
    gx->dirtyState |= 1;
}

#if defined(TARGET_PC)
// GXV_HOTPROF_REINITGX (hotprof25 round 2, 11 Nov; default 0; the switch: platform/src/port/perf.cpp; the caller: J3DSys::reinitTevStages,
// decomp/src/JSystem/J3DGraphBase/J3DSys.cpp). J3DSys::reinitGX runs 6.45 times a picture (the console's profile: 120,122 calls in 18,618
// pictures) and resets the 16 TEV stages through 128 GX calls: GXSetTevOrder(stage, null, null, null) for every stage, then per stage
// GXSetTevColorIn / ColorOp / AlphaIn / AlphaOp / KColorSel / KAlphaSel / SwapMode with constants; 144 BP writes, most of them of a
// register on its way to its final value. port_gx_reinit_tev does the same in one pass: the library's state as those calls leave it
// (the same bit-field macros on gx->tref, tevc, teva, tevKsel; texmapId, tevTcEnab, bpSentNot, dirtyState), and every register written
// once, with its final value. The front end's registers after it are the ones the 128 calls leave (a BP register holds its last write;
// none of these acts on a write), and nothing is drawn in between. Every register is still written: nothing is assumed about what the
// stream holds. Not while the GX capture layer is active (the calls must be recorded one by one), not into a display list being
// recorded, not while a particle loop is gathered, only while the FIFO feeds the vgx front end: then the caller makes the calls.
//   2: verify: the calls as before; port_gx_reinit_tev_verify compares the library's state after them with what the one pass makes of
//      the state before them ("reinitgx:" line every 4096 resets; mismatches must be 0).
int port_hotprof_reinitgx_mode(void) __attribute__((weak));
// (GXSetTevColorIn(stage, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO), then GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO,
// GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV): their statements, in their order)
static u32 PortReinitTevc(u32 tevReg) {
    SC_TEV_COLOR_ENV_SET_SELA(tevReg, GX_CC_RASC);
    SC_TEV_COLOR_ENV_SET_SELB(tevReg, GX_CC_ZERO);
    SC_TEV_COLOR_ENV_SET_SELC(tevReg, GX_CC_ZERO);
    SC_TEV_COLOR_ENV_SET_SELD(tevReg, GX_CC_ZERO);
    SC_TEV_COLOR_ENV_SET_SUB(tevReg, (GX_TEV_ADD & 1));
    SC_TEV_COLOR_ENV_SET_SHIFT(tevReg, GX_CS_SCALE_1);
    SC_TEV_COLOR_ENV_SET_BIAS(tevReg, GX_TB_ZERO);
    SC_TEV_COLOR_ENV_SET_CLAMP(tevReg, GX_TRUE);
    SC_TEV_COLOR_ENV_SET_DEST(tevReg, GX_TEVPREV);
    return tevReg;
}
// (GXSetTevAlphaIn(stage, GX_CA_RASA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO), GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
// GX_TRUE, GX_TEVPREV), GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0))
static u32 PortReinitTeva(u32 tevReg) {
    SC_TEV_ALPHA_ENV_SET_SELA(tevReg, GX_CA_RASA);
    SC_TEV_ALPHA_ENV_SET_SELB(tevReg, GX_CA_ZERO);
    SC_TEV_ALPHA_ENV_SET_SELC(tevReg, GX_CA_ZERO);
    SC_TEV_ALPHA_ENV_SET_SELD(tevReg, GX_CA_ZERO);
    SC_TEV_ALPHA_ENV_SET_SUB(tevReg, (GX_TEV_ADD & 1));
    SC_TEV_ALPHA_ENV_SET_SHIFT(tevReg, GX_CS_SCALE_1);
    SC_TEV_ALPHA_ENV_SET_BIAS(tevReg, GX_TB_ZERO);
    SC_TEV_ALPHA_ENV_SET_CLAMP(tevReg, GX_TRUE);
    SC_TEV_ALPHA_ENV_SET_DEST(tevReg, GX_TEVPREV);
    SC_TEV_ALPHA_ENV_SET_MODE(tevReg, GX_TEV_SWAP0);
    SC_TEV_ALPHA_ENV_SET_SWAP(tevReg, GX_TEV_SWAP0);
    return tevReg;
}
// The library's state after the 128 calls, made in *d.
static void PortReinitTevState(GXData* d) {
    u32 stage;
    for (stage = 0; stage < 16; stage++) {
        // (GXSetTevOrder(stage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL): tmap 0, the coordinate off, colour 7, texture off)
        u32* ptref = &d->tref[stage / 2];
        d->texmapId[stage] = GX_TEXMAP_NULL;
        d->tevTcEnab = d->tevTcEnab & ~(1 << stage);
        if (stage & 1) {
            SC_RAS1_TREF_SET_TI1(*ptref, GX_TEXMAP0);
            SC_RAS1_TREF_SET_TC1(*ptref, GX_TEXCOORD0);
            SC_RAS1_TREF_SET_CC1(*ptref, 7);
            SC_RAS1_TREF_SET_TE1(*ptref, 0);
        } else {
            SC_RAS1_TREF_SET_TI0(*ptref, GX_TEXMAP0);
            SC_RAS1_TREF_SET_TC0(*ptref, GX_TEXCOORD0);
            SC_RAS1_TREF_SET_CC0(*ptref, 7);
            SC_RAS1_TREF_SET_TE0(*ptref, 0);
        }
    }
    for (stage = 0; stage < 16; stage++) {
        u32* reg = &d->tevKsel[stage >> 1];
        d->tevc[stage] = PortReinitTevc(d->tevc[stage]);
        d->teva[stage] = PortReinitTeva(d->teva[stage]);
        if (stage & 1) {
            SC_TEV_KSEL_SET_KCSEL1((*reg), GX_TEV_KCSEL_1_4);
            SC_TEV_KSEL_SET_KASEL1((*reg), GX_TEV_KASEL_1);
        } else {
            SC_TEV_KSEL_SET_KCSEL0((*reg), GX_TEV_KCSEL_1_4);
            SC_TEV_KSEL_SET_KASEL0((*reg), GX_TEV_KASEL_1);
        }
    }
    d->bpSentNot = GX_FALSE;
    d->dirtyState |= 1;
}
int port_gx_reinit_ok(void) {
    return port_hotprof_reinitgx_mode && port_hotprof_reinitgx_mode() == 1 && !__PortGXCap && !__PortGXCapPass && !gx->inDispList && !__PortGXFx &&
           __PortGXRetainedOk && __PortGXRetainedOk();
}
// 1: the 16 stages are reset (J3DSys::reinitTevStages then skips its GXSetTevOrder calls and its loop). 0: the caller makes the calls.
int port_gx_reinit_tev(void) {
    u32 i;
    if (!port_gx_reinit_ok()) return 0;
    PortReinitTevState(gx);
    for (i = 0; i < 8; i++) GX_WRITE_RA_REG(gx->tref[i]);
    for (i = 0; i < 16; i++) {
        GX_WRITE_RA_REG(gx->tevc[i]);
        GX_WRITE_RA_REG(gx->teva[i]);
    }
    for (i = 0; i < 8; i++) GX_WRITE_RA_REG(gx->tevKsel[i]);
    return 1;
}
// Mode 2: before the calls (begin), and after the loop's last call (end).
static GXData sReinitBefore;
static int sReinitArmed;
static unsigned long long sReinitChecked, sReinitBad;
void port_gx_reinit_tev_verify(int end) {
    if (!port_hotprof_reinitgx_mode || port_hotprof_reinitgx_mode() != 2) return;
    if (!end) {
        memcpy(&sReinitBefore, gx, sizeof(GXData));
        sReinitArmed = !__PortGXCap && !__PortGXCapPass;  // (a capture: the calls are not made now; nothing to compare)
        return;
    }
    if (!sReinitArmed) return;
    sReinitArmed = 0;
    PortReinitTevState(&sReinitBefore);
    ++sReinitChecked;
    if (memcmp(&sReinitBefore, gx, sizeof(GXData)) != 0) {
        ++sReinitBad;
        if (sReinitBad <= 8 && port_log_summary) port_log_summary("reinitgx: MISMATCH: the one-pass reset would leave the GX library in another state than the calls did");
    }
    if ((sReinitChecked & 0xFFFu) == 0 && port_log_summary) {
        port_log_summary("reinitgx: GXV_HOTPROF_REINITGX=2: %llu resets of the TEV stages checked against the one-pass reset, %llu mismatches", sReinitChecked,
                         sReinitBad);
    }
}
#endif

void GXSetNumTevStages(u8 nStages) {
    PORT_GXCAP(GXSetNumTevStages, (nStages));
    SC_GEN_MODE_SET_NTEV(gx->genMode, (nStages - 1));
    gx->dirtyState |= 4;
}

// GXV_GX_DEDUP (gxdedup31, 18 Nov; default 0): the decision core of the producer-side removal of repeated register writes. Header only,
// plain C, no state of its own: the game's gate (platform/src/gx/gx_dedup.inc, behind the GX library's register-write macros) and the host
// fixture (tests/vgx_fe_work.cpp FEW_GXDEDUP) run the same code on their own PortGxDdShadow.
//
// What it decides. For each register write the producer is about to put into the top-level stream: "the front end's register already
// holds exactly this value, and nothing between depends on the write being there" (1: the write may be left out) or "send it" (0; the
// shadow then records it). The shadow is the producer's knowledge of what the stream has left in each register: the value of the last
// write it saw, valid while nothing the producer does not see may have written that register since.
//
// Which registers (the white lists below). Only registers whose write is a pure store in the vgx front end (vgx_state.cpp: WriteBP's
// generic tail and texture / TEV colour paths, WriteCP / WriteCPFull, WriteXFWord): a write of the value the register holds stores and
// marks nothing there. Never: the BP mask 0xFE (it applies to the NEXT BP write), draw done 0x45, the clock 0x46, the tokens 0x47 / 0x48,
// the EFB copy set-up and trigger 0x49..0x58 (0x52 triggers; the set-up is kept whole around it), the perf counters 0x23 / 0x24 / 0x67,
// the field / refresh registers 0x44 / 0x68 / 0x69, the TMEM preload / palette load registers 0x60..0x65 (0x63 and 0x65 load), the
// texture-cache invalidate 0x66, the copy filter 0x01..0x05, the port's markers 0xFF, and everything the GX spec leaves unused. CP: the
// matrix indices 0x30 / 0x40, the vertex descriptor 0x50 / 0x60, the vertex formats 0x70..0x97 (formats 0-7), the array bases and
// strides 0xA0..0xBF. XF: the clip mode 0x1005, the registers 0x1008..0x1019 (vertex spec, colours, channel controls, dual-tex, matrix
// indices) and the texgens 0x103F..0x1057. Not the viewport / projection block 0x101A..0x1026 (the GX library writes it as a raw block),
// no XF memory (matrices, lights: the indexed loads read guest memory) and nothing below 0x1005.
//
// One cell, two writers. The front end keeps a single matrix-index pair (State::matIdxA / B) that CP 0x30 / 0x40 and XF 0x1018 / 0x1019 both
// write (WriteCPFull, WriteXFWord). So CP 0x30 / 0x40 belong to the BP / XF family (forgotten with it), an XF write of 0x1018 / 0x1019 also
// sets the CP copy, and an XF matrix-index write is a candidate only while the CP copy holds the same word. (Found by the verify mode on the
// host fixture: a CP 0x30 write equal to the last CP 0x30 write, after an XF 0x1018 write of another value, changes the front end.)
//
// The BP mask. The front end applies a pending mask (BP 0xFE) to the next BP write of ANY register (markers too) and then forgets it. So
// a BP write is a candidate only while the producer KNOWS no mask is pending; the write after a mask is always sent and the shadow takes
// the masked result (or forgets the register when the old value was not known). CP and XF writes neither use nor change the mask.
//
// Invalidation ("bumps": every one is a call from the producer at the point of the stream where it happens):
//   PORT_GXDD_BUMP_ALL     a display-list call the producer cannot vouch for, the particle gathering's command, a port marker (a marker
//                          scope can put the front end's registers back), a display copy (a new picture), GXInit, a FIFO rebind, a
//                          write the producer made outside the stream: everything forgotten, the mask not known
//   PORT_GXDD_BUMP_NOCP    a J3D material or differed list (J3DDisplayListObj::callDL: J3D's GD writers put no CP command into them, the
//                          premise GXV_ARRAYSKIP already rests on; a mask they write is used by a BP write of the same list, J3DPortDL.hpp):
//                          BP and XF forgotten, CP kept; the mask known (none pending) when none was pending before the call, else unknown
//   PORT_GXDD_BUMP_PRIMS   a list of primitives and no-ops only (a J3D shape's geometry list, JParticle's quad lists): nothing forgotten
// A generation per family makes a bump O(1): an entry is valid when its tag is the family's generation (tags restart from 0 when a
// generation would wrap). Verify (GXV_GX_DEDUP=2) checks every candidate against the front end's register file, so a list that breaks
// the premise of its class shows up there.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PORT_GXDD_BUMP_ALL = 0, PORT_GXDD_BUMP_NOCP = 1, PORT_GXDD_BUMP_PRIMS = 2 };
enum { PORT_GXDD_MASK_NONE = 0, PORT_GXDD_MASK_PENDING = 1, PORT_GXDD_MASK_UNKNOWN = 2 };
enum { PORT_GXDD_XF_LO = 0x1000, PORT_GXDD_XF_N = 0x58 };

typedef struct PortGxDdShadow {
    uint32_t gen, genCp;         // the BP / XF generation, the CP generation (both from 1)
    uint32_t mask;               // PORT_GXDD_MASK_*
    uint32_t maskVal;            // the pending mask (PORT_GXDD_MASK_PENDING)
    uint32_t bp[256], bpTag[256];
    uint32_t cp[256], cpTag[256];
    uint32_t xf[PORT_GXDD_XF_N], xfTag[PORT_GXDD_XF_N];
    uint8_t okBp[256], okCp[256], okXf[PORT_GXDD_XF_N];
#if defined(PORT_GXDD_TEST_MUTANTS)
    int mutant;                  // (host tests only) 1: side-effect registers taken as plain, 2: the pending mask ignored, 3: bumps ignored, 4: every list primitives-only
#endif
} PortGxDdShadow;

// The white lists (see above).
static inline int PortGxDdBpPlain(uint32_t reg) {
    return reg == 0x00 || (reg >= 0x06 && reg <= 0x22) || (reg >= 0x25 && reg <= 0x43) || reg == 0x59 || (reg >= 0x80 && reg <= 0xBB) ||
           (reg >= 0xC0 && reg <= 0xFD);
}
static inline int PortGxDdCpPlain(uint32_t reg) {
    return reg == 0x30 || reg == 0x40 || reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97 && (reg & 0xFu) < 8u) || (reg >= 0xA0 && reg <= 0xBF);
}
static inline int PortGxDdXfPlain(uint32_t addr) {
    return addr == 0x1005 || (addr >= 0x1008 && addr <= 0x1019) || (addr >= 0x103F && addr <= 0x1057);
}

static inline void PortGxDdInit(PortGxDdShadow* s) {
    uint32_t i;
#if defined(PORT_GXDD_TEST_MUTANTS)
    const int mutant = s->mutant;
#endif
    memset(s, 0, sizeof(*s));
#if defined(PORT_GXDD_TEST_MUTANTS)
    s->mutant = mutant;
#endif
    s->gen = s->genCp = 1;
    s->mask = PORT_GXDD_MASK_UNKNOWN;
    for (i = 0; i < 256; ++i) {
        s->okBp[i] = (uint8_t)PortGxDdBpPlain(i);
        s->okCp[i] = (uint8_t)PortGxDdCpPlain(i);
    }
    for (i = 0; i < PORT_GXDD_XF_N; ++i) s->okXf[i] = (uint8_t)PortGxDdXfPlain(PORT_GXDD_XF_LO + i);
#if defined(PORT_GXDD_TEST_MUTANTS)
    if (s->mutant == 1) { s->okBp[0x45] = s->okBp[0x47] = s->okBp[0x48] = s->okBp[0x52] = s->okBp[0x65] = s->okBp[0x66] = s->okBp[0x63] = 1; }
#endif
}

static inline void PortGxDdBump(PortGxDdShadow* s, int kind) {
#if defined(PORT_GXDD_TEST_MUTANTS)
    if (s->mutant == 3) return;
    if (s->mutant == 4) kind = PORT_GXDD_BUMP_PRIMS;
#endif
    if (kind == PORT_GXDD_BUMP_PRIMS) return;
    if (++s->gen == 0) {
        memset(s->bpTag, 0, sizeof(s->bpTag));
        memset(s->xfTag, 0, sizeof(s->xfTag));
        s->cpTag[0x30] = s->cpTag[0x40] = 0;
        s->gen = 1;
    }
    if (kind == PORT_GXDD_BUMP_ALL) {
        if (++s->genCp == 0) {
            memset(s->cpTag, 0, sizeof(s->cpTag));
            s->genCp = 1;
        }
        s->mask = PORT_GXDD_MASK_UNKNOWN;
    } else if (s->mask != PORT_GXDD_MASK_NONE) {
        // (a mask pending or unknown before the list: the list's first BP write uses it, or a list of XF writes only leaves it pending;
        // either way not known now)
        s->mask = PORT_GXDD_MASK_UNKNOWN;
    }
}

// A BP write `rv` (register << 24 | value). 1: a candidate (the register holds the value, no mask pending: the stream is the same without
// it); 0: it must be sent, and the shadow now describes the stream after it.
static inline int PortGxDdBp(PortGxDdShadow* s, uint32_t rv) {
    const uint32_t reg = rv >> 24, v = rv & 0x00FFFFFFu;
    if (reg == 0xFE) {
        s->mask = PORT_GXDD_MASK_PENDING;
        s->maskVal = v;
        return 0;
    }
#if defined(PORT_GXDD_TEST_MUTANTS)
    if (s->mutant == 2) s->mask = PORT_GXDD_MASK_NONE;
#endif
    if (s->mask != PORT_GXDD_MASK_NONE) {  // (this write uses the pending mask, if any: sent; then none is pending)
        if (s->okBp[reg]) {
            if (s->mask == PORT_GXDD_MASK_PENDING && s->bpTag[reg] == s->gen) s->bp[reg] = (s->bp[reg] & ~s->maskVal) | (v & s->maskVal);
            else s->bpTag[reg] = 0;
        }
        s->mask = PORT_GXDD_MASK_NONE;
        return 0;
    }
    if (!s->okBp[reg]) return 0;
    if (s->bpTag[reg] == s->gen && s->bp[reg] == v) return 1;
    s->bp[reg] = v;
    s->bpTag[reg] = s->gen;
    return 0;
}

static inline int PortGxDdCp(PortGxDdShadow* s, uint32_t reg, uint32_t v) {
    reg &= 0xFFu;
    if (!s->okCp[reg]) return 0;
    if (reg == 0x30 || reg == 0x40) {  // (the matrix indices: one cell with XF 0x1018 / 0x1019 in the front end, so of the BP / XF family)
        if (s->cpTag[reg] == s->gen && s->cp[reg] == v) return 1;
        s->cp[reg] = v;
        s->cpTag[reg] = s->gen;
        return 0;
    }
    if (s->cpTag[reg] == s->genCp && s->cp[reg] == v) return 1;
    s->cp[reg] = v;
    s->cpTag[reg] = s->genCp;
    return 0;
}

// An XF load of n words at addr (one command). 1: every word is a white-listed register that holds its word (the command may be left out);
// 0: sent, and the shadow takes every white-listed word of it (a candidate has n up to 16; the rest of XF is not kept).
static inline int PortGxDdXf(PortGxDdShadow* s, uint32_t addr, uint32_t n, const uint32_t* w) {
    uint32_t i;
    int all = n >= 1 && n <= 16;
    for (i = 0; i < n && all; ++i) {
        const uint32_t a = addr + i;
        if (a < PORT_GXDD_XF_LO || a >= PORT_GXDD_XF_LO + PORT_GXDD_XF_N) { all = 0; break; }
        if (!s->okXf[a - PORT_GXDD_XF_LO] || s->xfTag[a - PORT_GXDD_XF_LO] != s->gen || s->xf[a - PORT_GXDD_XF_LO] != w[i]) all = 0;
        else if ((a == 0x1018 || a == 0x1019) && (s->cpTag[a == 0x1018 ? 0x30 : 0x40] != s->gen || s->cp[a == 0x1018 ? 0x30 : 0x40] != w[i])) all = 0;
    }
    if (all) return 1;
    for (i = 0; i < n; ++i) {  // (w holds all n words)
        const uint32_t a = addr + i;
        if (a >= PORT_GXDD_XF_LO && a < PORT_GXDD_XF_LO + PORT_GXDD_XF_N && s->okXf[a - PORT_GXDD_XF_LO]) {
            s->xf[a - PORT_GXDD_XF_LO] = w[i];
            s->xfTag[a - PORT_GXDD_XF_LO] = s->gen;
            if (a == 0x1018 || a == 0x1019) {  // (the front end's matrix-index cell, which CP 0x30 / 0x40 write too)
                s->cp[a == 0x1018 ? 0x30 : 0x40] = w[i];
                s->cpTag[a == 0x1018 ? 0x30 : 0x40] = s->gen;
            }
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

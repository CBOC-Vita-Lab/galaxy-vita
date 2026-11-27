// GXV_GX_DEDUP / GXV_GX_DEDUP_CENSUS (gxdedup31, 18 Nov; default 0): the gate behind the GX library's register-write macros (the switches
// and their lines: platform/include/port/gx_dedup.h; what is decided and why it is exact: platform/include/port/gx_dedup_core.h).
// Included into gx_fifo.cpp, after the staging buffer's state (the gate is live only while the FIFO writes into it). Game thread.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "port/gx_dedup.h"

extern "C" {
#if !defined(PORT_GX_DEDUP_WEAK)
unsigned char __PortGXDedupOn = 0;
unsigned char __PortGXDedupOnBP = 0;
unsigned char __PortGXDedupOnCX = 0;
unsigned char __PortGXDedupKeep = 0;
#endif
void port_log_summary(const char* fmt, ...) __attribute__((weak));
}

namespace {
PortGxDdShadow sDd;
int sDdMode = 0;       // GXV_GX_DEDUP: 0 count only, 1 leave out, 2 verify
int sDdCensus = 0;     // GXV_GX_DEDUP_CENSUS: pictures a census window has (0 off)
bool sDdReady = false;
// GXV_GX_DEDUP=1 without the census: the gate's own short path (below, at the top of each gate). It makes the decision of the core
// (port/gx_dedup_core.h PortGxDdBp / Cp / Xf) for the common case (a white-listed register, no BP mask pending or unknown, the FIFO
// writing the stream) in a few loads and compares, so that a write it leaves out costs the game thread about what the write itself cost,
// and a write it sends little more; every other case takes the general path. The two counts are 32-bit (folded into the line's window).
bool sDdFast = false;
int sDdClasses = 3;    // GXV_GX_DEDUP_CLASSES
uint32_t sDdFastSent[3] = {}, sDdFastSame[3] = {};  // (by class: BP, CP, XF; one count on each of its two outcomes)

// ---- the counts (the "gxdedup:" line every 300 pictures while the switch is on; the census's lines)
enum DdCls { kDdBp, kDdCp, kDdXf, kDdClasses };
const char* const kDdClsName[kDdClasses] = {"BP", "CP", "XF"};
enum DdGrp : uint8_t {
    gTevStage, gTevColor, gTex, gTexCoord, gPe, gInd, gGen, gFog, gMask, gNever, gBpOther,
    gCpVcd, gCpArr, gCpMtxIdx, gXfMtxIdx, gXfTexgen, gXfChan, gXfOther, kDdGroups
};
const char* const kDdGrpName[kDdGroups] = {"tev-stage", "tev-colour", "tex", "texcoord-scale", "pixel", "indirect", "genmode", "fog", "bp-mask",
                                           "side-effect", "bp-other", "vtx-desc/fmt", "array-base/stride", "cp-mtxidx", "xf-mtxidx", "texgen",
                                           "chan-colour/ctrl", "xf-other"};
inline DdGrp DdBpGroup(uint32_t reg) {
    if (reg == 0xFE) return gMask;
    if (!PortGxDdBpPlain(reg)) return gNever;
    if (reg >= 0xE0 && reg <= 0xE7) return gTevColor;
    if ((reg >= 0xC0 && reg <= 0xDF) || (reg >= 0x28 && reg <= 0x2F) || (reg >= 0xF6 && reg <= 0xFD)) return gTevStage;
    if (reg >= 0x80 && reg <= 0xBB) return gTex;
    if (reg >= 0x30 && reg <= 0x3F) return gTexCoord;
    if ((reg >= 0x06 && reg <= 0x1F) || (reg >= 0x25 && reg <= 0x27)) return gInd;
    if (reg == 0x00) return gGen;
    if (reg >= 0xE8 && reg <= 0xF2) return gFog;
    if ((reg >= 0x20 && reg <= 0x22) || (reg >= 0x40 && reg <= 0x43) || reg == 0x59 || reg == 0xF3) return gPe;
    return gBpOther;
}
inline DdGrp DdCpGroup(uint32_t reg) {
    if (!PortGxDdCpPlain(reg)) return gNever;
    if (reg >= 0xA0) return gCpArr;
    if (reg == 0x30 || reg == 0x40) return gCpMtxIdx;
    return gCpVcd;
}
inline DdGrp DdXfGroup(uint32_t a) {
    if (!PortGxDdXfPlain(a)) return gNever;
    if (a == 0x1018 || a == 0x1019) return gXfMtxIdx;
    if (a >= 0x103F) return gXfTexgen;
    if (a >= 0x1009 && a <= 0x1011) return gXfChan;
    return gXfOther;
}
struct DdSite { const void* ra; uint32_t writes, same, lost; };
constexpr int kDdSites = 1024;  // (open addressing, 16 probes; a site that finds no slot is counted as lost)
struct DdWin {
    uint32_t pics = 0;
    uint64_t w[kDdClasses] = {}, same[kDdClasses] = {}, never[kDdClasses] = {}, outside[kDdClasses] = {}, saved = 0;
    uint64_t gw[kDdGroups] = {}, gs[kDdGroups] = {}, gl[kDdGroups] = {};
    uint64_t lost[PORT_GXDD_CAUSES + 2] = {};  // (+0: by an invalidation of each cause, +CAUSES: the write after a mask, +CAUSES+1: older than the ring)
    uint64_t bumps[PORT_GXDD_CAUSES] = {};
    uint64_t masked = 0, sitesLost = 0;
};
DdWin sDdWin, sDdLine;            // the census window, the mode line's window
DdSite* sDdSiteTab = nullptr;     // (the census only)
// the cause of each invalidation, by the generation it ended (for "forgotten by"): BP / XF and CP families
uint8_t sDdBumpBx[4096], sDdBumpCp[4096];
// the last value written at top level whatever was forgotten since (the census's "forgotten" test)
uint32_t sDdLastBp[256], sDdLastCp[256], sDdLastXf[PORT_GXDD_XF_N];
uint8_t sDdLastBpOk[256], sDdLastCpOk[256], sDdLastXfOk[PORT_GXDD_XF_N];

// The FIFO writes into the staging buffer the front end reads (the chain of the late draw, a capture, verify's scratch, a display list being
// recorded and the overflow sink all lie outside it). The engines that copy bytes into the stream are refused at the switch's read.
inline bool DdLive() {
    return sLinked && reinterpret_cast<uintptr_t>(__PortGXFifoPtr) - reinterpret_cast<uintptr_t>(sStaging) < kStagingSize
#if !defined(PORT_GX_VGX_ONLY)
           && port_vgx_active()
#endif
        ;
}
inline uintptr_t DdAnchor() { return reinterpret_cast<uintptr_t>(&GXInitFifoBase); }
DdSite* DdSiteOf(const void* ra) {
    if (!sDdSiteTab) return nullptr;
    uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(ra) >> 1) * 2654435761u) >> 22;
    for (int k = 0; k < 16; ++k, h = (h + 1) & (kDdSites - 1)) {
        DdSite& s = sDdSiteTab[h];
        if (s.ra == ra) return &s;
        if (!s.ra) { s.ra = ra; return &s; }
    }
    ++sDdWin.sitesLost;
    return nullptr;
}
// one tracked write's counts: same = a candidate; lostCause >= 0: identical to the last value but forgotten (that cause)
// (the census only; the mode line's two counts are made by the gate itself: DdLineNote)
void DdNote(int cls, DdGrp gr, bool same, int lostCause, const void* ra, uint32_t bytes) {
    DdWin* const w = &sDdWin;
    ++w->w[cls];
    ++w->gw[gr];
    if (gr == gNever || gr == gMask) ++w->never[cls];
    if (same) { ++w->same[cls]; ++w->gs[gr]; if (sDdMode == 1) w->saved += bytes; }
    if (lostCause >= 0) { ++w->lost[lostCause]; ++w->gl[gr]; }
    {
        if (DdSite* s = DdSiteOf(ra)) {
            ++s->writes;
            if (same) ++s->same;
            if (lostCause >= 0) ++s->lost;
        }
    }
}
// the "gxdedup:" line's counts: every tracked write, the candidates, the bytes left out (a few adds: the switch's own hot path)
inline void DdLineNote(int cls, bool same, uint32_t bytes) {
    ++sDdLine.w[cls];
    if (same) {
        ++sDdLine.same[cls];
        if (sDdMode == 1) sDdLine.saved += bytes;
    }
}
// why a register whose tag is `tag` (not the generation `gen`) was forgotten
inline int DdLostCause(uint32_t tag, uint32_t gen, const uint8_t* ring) {
    if (tag == 0) return PORT_GXDD_CAUSES;               // (the write after a mask that could not be followed, or never valid)
    if (gen - tag > 4000u) return PORT_GXDD_CAUSES + 1;
    return ring[tag & 4095u];
}

// ---- the writes as they were (the gate's own output)
inline void DdRawBP(uint32_t rv) {
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32(rv);
}
inline void DdRawCP(uint32_t reg, uint32_t v) { __PortGXFifo_op_u8_u32(0x08, static_cast<uint8_t>(reg), v); }
inline void DdRawXF(uint32_t hdr, uint32_t v) { __PortGXFifo_op_u32_u32(0x10, hdr, v); }
inline void DdRawXF2(uint32_t addr, uint32_t a, uint32_t b) {
    __PortGXFifo_op_u16_u16(0x10, 1, static_cast<uint16_t>(addr));
    __PortGXFifo_u32(a);
    __PortGXFifo_u32(b);
}
// GXV_GX_DEDUP=2: the probe in front of a write =1 leaves out (XF 0x0C00: class << 24 | register or address, 0x0C01: the value)
inline void DdProbe(uint32_t cls, uint32_t reg, uint32_t v) {
    __PortGXFifo_op_u16_u16(0x10, 1, 0x0C00);
    __PortGXFifo_u32((cls << 24) | (reg & 0xFFFFu));
    __PortGXFifo_u32(v);
}
// a write the gate cannot vouch for (the FIFO is not the stream now): it goes out as before and everything is forgotten
void DdOutside(int cls) {
    ++sDdWin.outside[cls];
    ++sDdLine.outside[cls];
    __PortGXDedupBumpAt(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_OUTSIDE);
}

void DdPrintCensus() {
    DdWin& w = sDdWin;
    const double P = w.pics ? static_cast<double>(w.pics) : 1.0;
    char line[2048];
    int o = 0;
#define DDADD(...) do { if (o < static_cast<int>(sizeof line) - 1) o += std::snprintf(line + o, sizeof line - o, __VA_ARGS__); } while (0)
    auto out = [&]() {
        if (port_log_summary) port_log_summary("%s", line);
        else port_log("%s", line);
        o = 0;
        line[0] = 0;
    };
    uint64_t tw = 0, ts = 0, tl = 0;
    for (int c = 0; c < kDdClasses; ++c) { tw += w.w[c]; ts += w.same[c]; }
    for (int k = 0; k < PORT_GXDD_CAUSES + 2; ++k) tl += w.lost[k];
    DDADD("gxdedup-census: %u pictures (GXV_GX_DEDUP=%d), per picture: tracked top-level register writes %.0f, identical to the shadow %.0f "
          "(%s), identical to the last value but forgotten %.0f; by class writes:identical (side-effect or mask, not in the stream):",
          w.pics, sDdMode, tw / P, ts / P, sDdMode == 1 ? "left out" : sDdMode == 2 ? "sent behind a probe" : "count only", tl / P);
    for (int c = 0; c < kDdClasses; ++c) DDADD(" %s %.0f:%.0f (%.0f, %.1f)", kDdClsName[c], w.w[c] / P, w.same[c] / P, w.never[c] / P, w.outside[c] / P);
    DDADD("; BP writes while a mask is or may be pending (always sent) %.1f", w.masked / P);
    out();
    DDADD("gxdedup-census: by group writes:identical:forgotten per picture:");
    for (int g = 0; g < kDdGroups; ++g)
        if (w.gw[g]) DDADD(" %s %.0f:%.0f:%.0f", kDdGrpName[g], w.gw[g] / P, w.gs[g] / P, w.gl[g] / P);
    out();
    static const char* const kCause[PORT_GXDD_CAUSES + 2] = {"list-call", "material-list", "prims-list", "marker", "picture", "init", "fifo",
                                                              "not-stream", "other", "after-mask", "older"};
    DDADD("gxdedup-census: invalidations per picture (forgotten writes they cost):");
    for (int k = 0; k < PORT_GXDD_CAUSES + 2; ++k)
        if ((k < PORT_GXDD_CAUSES && w.bumps[k]) || w.lost[k]) DDADD(" %s %.1f (%.0f)", kCause[k], k < PORT_GXDD_CAUSES ? w.bumps[k] / P : 0.0, w.lost[k] / P);
    out();
    if (sDdSiteTab) {
        std::vector<int> idx;
        for (int i = 0; i < kDdSites; ++i) if (sDdSiteTab[i].ra) idx.push_back(i);
        for (int pass = 0; pass < 2; ++pass) {
            std::sort(idx.begin(), idx.end(), [&](int a, int b) {
                return pass == 0 ? sDdSiteTab[a].same > sDdSiteTab[b].same : sDdSiteTab[a].lost > sDdSiteTab[b].lost;
            });
            DDADD("gxdedup-census: call sites by %s (return address as GXInitFifoBase%c offset: writes:identical:forgotten per picture; %u sites, %llu not kept):",
                  pass == 0 ? "identical writes" : "forgotten writes", '+', static_cast<unsigned>(idx.size()), static_cast<unsigned long long>(w.sitesLost));
            for (size_t j = 0; j < idx.size() && j < 14; ++j) {
                const DdSite& s = sDdSiteTab[idx[j]];
                const intptr_t off = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(s.ra) - DdAnchor());
                DDADD(" %c0x%lx %.0f:%.0f:%.0f;", off < 0 ? '-' : '+', static_cast<unsigned long>(off < 0 ? -off : off), s.writes / P, s.same / P, s.lost / P);
            }
            out();
        }
        std::memset(sDdSiteTab, 0, sizeof(DdSite) * kDdSites);
    }
#undef DDADD
    w = DdWin{};
}
void DdPrintLine() {
    DdWin& w = sDdLine;
    static const uint32_t kBytes[kDdClasses] = {5, 6, 9};
    for (int c = 0; c < kDdClasses; ++c) {  // (the short path's counts)
        w.w[c] += sDdFastSent[c] + sDdFastSame[c];
        w.same[c] += sDdFastSame[c];
        w.saved += static_cast<uint64_t>(sDdFastSame[c]) * kBytes[c];
        sDdFastSent[c] = sDdFastSame[c] = 0;
    }
    const double P = w.pics ? static_cast<double>(w.pics) : 1.0;
    uint64_t tw = 0, ts = 0, tb = 0;
    for (int c = 0; c < kDdClasses; ++c) { tw += w.w[c]; ts += w.same[c]; }
    for (int k = 0; k < PORT_GXDD_CAUSES; ++k) tb += w.bumps[k];
    const char* const fmt = "gxdedup: GXV_GX_DEDUP=%d (classes %d), %u pictures: per picture %.0f tracked register writes, %.0f %s (BP %.0f, CP %.0f, XF %.0f), "
                            "%.0f bytes saved, %.1f invalidations (lists %.1f, material lists %.1f), %.1f writes outside the stream";
    const double outside = (w.outside[0] + w.outside[1] + w.outside[2]) / P;
    const char* what = sDdMode == 1 ? "left out" : "sent behind a probe (see the front end's \"gxdedup verify:\")";
    if (port_log_summary)
        port_log_summary(fmt, sDdMode, sDdClasses, w.pics, tw / P, ts / P, what, w.same[0] / P, w.same[1] / P, w.same[2] / P, w.saved / P, tb / P,
                         w.bumps[PORT_GXDD_CAUSE_LIST] / P, w.bumps[PORT_GXDD_CAUSE_MATLIST] / P, outside);
    else
        port_log(fmt, sDdMode, sDdClasses, w.pics, tw / P, ts / P, what, w.same[0] / P, w.same[1] / P, w.same[2] / P, w.saved / P, tb / P,
                 w.bumps[PORT_GXDD_CAUSE_LIST] / P, w.bumps[PORT_GXDD_CAUSE_MATLIST] / P, outside);
    w = DdWin{};
}
}  // namespace

extern "C" void port_gx_dedup_init(void) {
    if (sDdReady) return;
    sDdReady = true;
    const int saved = errno;
    long mode = vgx::EnvInt("GXV_GX_DEDUP", 0);
    long census = vgx::EnvInt("GXV_GX_DEDUP_CENSUS", 0);
    if (mode < 0 || mode > 2) mode = 0;
    if (census < 0) census = 0;
    if (census == 1) census = 300;
#if !(defined(PORT_GX_DEDUP_BUILD) && PORT_GX_DEDUP_BUILD)
    if (mode != 0 || census != 0) {  // (the writes do not go through the gate in this build: port/gx_dedup.h)
        port_log("gx: GXV_GX_DEDUP=%ld / GXV_GX_DEDUP_CENSUS=%ld: the gate is not in this build (PORT_GX_DEDUP_BUILD=1 builds it in); nothing changes",
                 mode, census);
        mode = census = 0;
    }
#endif
    if (mode != 0) {
        static const char* const kNo[] = {"GXV_REPLAY25", "GXV_PIPE25", "GXV_GXCAPTURE", "GXV_FXMERGE", "GXV_DRAWPIPE"};
        for (const char* n : kNo) {
            if (vgx::EnvInt(n, 0) != 0) {
                port_log("gx: GXV_GX_DEDUP=%ld refused: %s is on (it copies bytes into the stream or reorders it); the switch stays 0", mode, n);
                mode = 0;
                break;
            }
        }
    }
    errno = saved;
    sDdMode = static_cast<int>(mode);
    sDdCensus = static_cast<int>(census);
    PortGxDdInit(&sDd);
    if (sDdCensus) sDdSiteTab = static_cast<DdSite*>(calloc(kDdSites, sizeof(DdSite)));
    __PortGXDedupOn = sDdCensus != 0 ? 2 : sDdMode != 0 ? 1 : 0;
    long classes = sDdCensus ? 3 : vgx::EnvInt("GXV_GX_DEDUP_CLASSES", 3);
    if (classes < 1 || classes > 3) classes = 3;
    sDdClasses = static_cast<int>(classes);
    __PortGXDedupOnBP = (__PortGXDedupOn && (classes & 1)) ? 1 : 0;
    __PortGXDedupOnCX = (__PortGXDedupOn && (classes & 2)) ? 1 : 0;
    sDdFast = sDdMode == 1 && sDdCensus == 0 && vgx::EnvInt("GXV_GX_DEDUP_SHORT", 1) != 0;  // (GXV_GX_DEDUP_SHORT=0: the general path only)
    if (__PortGXDedupOn) port_log("gx: repeated register writes (GXV_GX_DEDUP=%d, census %d, classes %s): %s", sDdMode, sDdCensus,
                                  sDdClasses == 3 ? "BP, CP, XF" : sDdClasses == 1 ? "BP" : "CP, XF",
                                  sDdMode == 1 ? "left out at the source" : sDdMode == 2 ? "verify: sent behind probes" : "counted only");
}

extern "C" void __PortGXDedupBumpAt(int kind, int cause) {
    if (kind != PORT_GXDD_BUMP_PRIMS) {
        sDdBumpBx[sDd.gen & 4095u] = static_cast<uint8_t>(cause);
        if (kind == PORT_GXDD_BUMP_ALL) sDdBumpCp[sDd.genCp & 4095u] = static_cast<uint8_t>(cause);
    }
    PortGxDdBump(&sDd, kind);
    if (cause >= 0 && cause < PORT_GXDD_CAUSES) { ++sDdWin.bumps[cause]; ++sDdLine.bumps[cause]; }
    if (cause == PORT_GXDD_CAUSE_PICTURE) {
        if (sDdCensus && ++sDdWin.pics >= static_cast<uint32_t>(sDdCensus)) DdPrintCensus();
        if (sDdMode != 0 && ++sDdLine.pics >= 300u) DdPrintLine();
    }
}

namespace {
// The general path of each gate (every mode, the census, the BP mask, the matrix indices, a FIFO that is not the stream); the extern
// gates below are the short path, which has no call in it, so that the compiler saves no register for the general one.
__attribute__((noinline)) void DdBPSlow(uint32_t rv, const void* ra) {
    if (!DdLive()) {
        DdRawBP(rv);
        DdOutside(kDdBp);
        return;
    }
    const uint32_t reg = rv >> 24, v = rv & 0x00FFFFFFu;
    const uint32_t maskBefore = sDd.mask, tagBefore = sDd.bpTag[reg];
    const bool same = PortGxDdBp(&sDd, rv) != 0;
    DdLineNote(kDdBp, same, 5);
    if (__builtin_expect(sDdCensus != 0, 0)) {
        int lost = -1;
        if (!same && reg != 0xFE && sDd.okBp[reg] && maskBefore == PORT_GXDD_MASK_NONE && sDdLastBpOk[reg] && sDdLastBp[reg] == v && tagBefore != sDd.gen)
            lost = DdLostCause(tagBefore, sDd.gen, sDdBumpBx);
        if (maskBefore != PORT_GXDD_MASK_NONE && reg != 0xFE) ++sDdWin.masked;
        DdNote(kDdBp, DdBpGroup(reg), same, lost, ra, 5);
        if (reg != 0xFE && maskBefore == PORT_GXDD_MASK_NONE) { sDdLastBp[reg] = v; sDdLastBpOk[reg] = 1; }
        else if (reg != 0xFE) sDdLastBpOk[reg] = 0;
    }
    if (same) {
        if (sDdMode == 1) return;
        if (sDdMode == 2) DdProbe(0, reg, v);
    }
    DdRawBP(rv);
}
}  // namespace
// GXV_GX_DEDUP=1 without the census (sDdFast): PortGxDdBp's plain case (a white-listed register, no mask pending or unknown, the FIFO
// writing the stream) decided here; the write it sends goes out as one batch when the element-wise form would not have reached the
// FIFO's limit (the same bytes and write pointer), else the general path writes it element by element.
extern "C" void __PortGXDedupBP(uint32_t rv, const void* ra) {
    if (__builtin_expect(sDdFast, 1)) {
        const uint32_t reg = rv >> 24, v = rv & 0x00FFFFFFu;
        uint8_t* const p = __PortGXFifoPtr;
        if (sDd.okBp[reg] && sDd.mask == PORT_GXDD_MASK_NONE && sLinked && reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(sStaging) < kStagingSize) {
            if (sDd.bpTag[reg] == sDd.gen && sDd.bp[reg] == v) {
                ++sDdFastSame[kDdBp];
                return;
            }
            if (static_cast<intptr_t>(reinterpret_cast<uintptr_t>(__PortGXFifoLimit) - reinterpret_cast<uintptr_t>(p)) > 5) {
                ++sDdFastSent[kDdBp];
                sDd.bp[reg] = v;
                sDd.bpTag[reg] = sDd.gen;
                __PortGXFifoCommit(__PortGXPut_u32(__PortGXPut_u8(p, 0x61), rv));
                return;
            }
        }
    }
    DdBPSlow(rv, ra);
}

namespace {
__attribute__((noinline)) void DdCPSlow(uint32_t reg, uint32_t v, const void* ra) {
    if (!DdLive()) {
        DdRawCP(reg, v);
        DdOutside(kDdCp);
        return;
    }
    const uint32_t tagBefore = sDd.cpTag[reg];
    const bool same = PortGxDdCp(&sDd, reg, v) != 0;
    DdLineNote(kDdCp, same, 6);
    if (__builtin_expect(sDdCensus != 0, 0)) {
        int lost = -1;
        const bool bx = reg == 0x30 || reg == 0x40;  // (the matrix indices are of the BP / XF family: gx_dedup_core.h)
        const uint32_t gen = bx ? sDd.gen : sDd.genCp;
        if (!same && sDd.okCp[reg] && sDdLastCpOk[reg] && sDdLastCp[reg] == v && tagBefore != gen) lost = DdLostCause(tagBefore, gen, bx ? sDdBumpBx : sDdBumpCp);
        DdNote(kDdCp, DdCpGroup(reg), same, lost, ra, 6);
        sDdLastCp[reg] = v;
        sDdLastCpOk[reg] = 1;
    }
    if (same) {
        if (sDdMode == 1) return;
        if (sDdMode == 2) DdProbe(1, reg, v);
    }
    DdRawCP(reg, v);
}
}  // namespace
// (the short path: PortGxDdCp; as __PortGXDedupBP)
extern "C" void __PortGXDedupCP(uint32_t reg, uint32_t v, const void* ra) {
    reg &= 0xFFu;
    uint8_t* const p = __PortGXFifoPtr;
    if (__builtin_expect(sDdFast, 1) && sDd.okCp[reg] && sLinked && reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(sStaging) < kStagingSize) {
        const uint32_t gen = (reg == 0x30 || reg == 0x40) ? sDd.gen : sDd.genCp;
        if (sDd.cpTag[reg] == gen && sDd.cp[reg] == v) {
            ++sDdFastSame[kDdCp];
            return;
        }
        if (static_cast<intptr_t>(reinterpret_cast<uintptr_t>(__PortGXFifoLimit) - reinterpret_cast<uintptr_t>(p)) > 6) {
            ++sDdFastSent[kDdCp];
            sDd.cp[reg] = v;
            sDd.cpTag[reg] = gen;
            __PortGXFifoCommit(__PortGXPut_u32(__PortGXPut_u8(__PortGXPut_u8(p, 0x08), static_cast<uint8_t>(reg)), v));
            return;
        }
    }
    DdCPSlow(reg, v, ra);
}

namespace {
// the census's view of an XF write of n (1 or 2) words: the group of its first word, "forgotten" when every word was the last value
void DdNoteXf(uint32_t addr, uint32_t n, const uint32_t* w, const uint32_t* tagsBefore, bool same, const void* ra) {
    int lost = -1;
    if (!same) {
        bool all = true;
        int stale = -1;
        for (uint32_t i = 0; i < n && all; ++i) {
            const uint32_t a = addr + i;
            if (a < PORT_GXDD_XF_LO || a >= PORT_GXDD_XF_LO + PORT_GXDD_XF_N) { all = false; break; }
            const uint32_t k = a - PORT_GXDD_XF_LO;
            if (!sDd.okXf[k] || !sDdLastXfOk[k] || sDdLastXf[k] != w[i]) all = false;
            else if (stale < 0 && tagsBefore[i] != sDd.gen) stale = static_cast<int>(i);
        }
        if (all && stale >= 0) lost = DdLostCause(tagsBefore[stale], sDd.gen, sDdBumpBx);
    }
    DdNote(kDdXf, DdXfGroup(addr), same, lost, ra, n == 1 ? 9u : 13u);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t a = addr + i;
        if (a >= PORT_GXDD_XF_LO && a < PORT_GXDD_XF_LO + PORT_GXDD_XF_N) { sDdLastXf[a - PORT_GXDD_XF_LO] = w[i]; sDdLastXfOk[a - PORT_GXDD_XF_LO] = 1; }
    }
}
inline uint32_t DdXfTag(uint32_t a) { return a >= PORT_GXDD_XF_LO && a < PORT_GXDD_XF_LO + PORT_GXDD_XF_N ? sDd.xfTag[a - PORT_GXDD_XF_LO] : 0u; }
}  // namespace

namespace {
__attribute__((noinline)) void DdXFSlow(uint32_t hdr, uint32_t v, const void* ra) {
    const uint32_t addr = hdr & 0xFFFFu, cnt = (hdr >> 16) & 0xFu;
    if (!DdLive() || cnt != 0) {  // (cnt != 0: a header for more words than the one this macro writes: never seen, sent as it is)
        DdRawXF(hdr, v);
        if (cnt != 0) {  // (the XF registers forgotten, the mask state kept: an XF write neither uses nor changes it)
            const uint32_t mask = sDd.mask;
            __PortGXDedupBumpAt(PORT_GXDD_BUMP_NOCP, PORT_GXDD_CAUSE_OTHER);
            sDd.mask = mask;
        } else {
            DdOutside(kDdXf);
        }
        return;
    }
    const uint32_t tag = DdXfTag(addr);
    const bool same = PortGxDdXf(&sDd, addr, 1, &v) != 0;
    DdLineNote(kDdXf, same, 9);
    if (__builtin_expect(sDdCensus != 0, 0)) DdNoteXf(addr, 1, &v, &tag, same, ra);
    if (same) {
        if (sDdMode == 1) return;
        if (sDdMode == 2) DdProbe(2, addr, v);
    }
    DdRawXF(hdr, v);
}
}  // namespace
// (the short path: PortGxDdXf of one word; as __PortGXDedupBP. A matrix index, 0x1018 / 0x1019, shares its cell with CP 0x30 / 0x40:
// a candidate only while that copy holds the word too, and a write sets it)
extern "C" void __PortGXDedupXF(uint32_t hdr, uint32_t v, const void* ra) {
    const uint32_t k = (hdr & 0xFFFFu) - PORT_GXDD_XF_LO;  // (hdr's count field is 0: then hdr is the address; else k is out of range)
    uint8_t* const p = __PortGXFifoPtr;
    if (__builtin_expect(sDdFast, 1) && (hdr >> 16) == 0 && k < PORT_GXDD_XF_N && sDd.okXf[k] && sLinked &&
        reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(sStaging) < kStagingSize) {
        const bool mtx = (k & ~1u) == 0x18u;
        const uint32_t cpReg = (k == 0x18u) ? 0x30u : 0x40u;
        if (sDd.xfTag[k] == sDd.gen && sDd.xf[k] == v && (!mtx || (sDd.cpTag[cpReg] == sDd.gen && sDd.cp[cpReg] == v))) {
            ++sDdFastSame[kDdXf];
            return;
        }
        if (static_cast<intptr_t>(reinterpret_cast<uintptr_t>(__PortGXFifoLimit) - reinterpret_cast<uintptr_t>(p)) > 9) {
            ++sDdFastSent[kDdXf];
            sDd.xf[k] = v;
            sDd.xfTag[k] = sDd.gen;
            if (mtx) {
                sDd.cp[cpReg] = v;
                sDd.cpTag[cpReg] = sDd.gen;
            }
            __PortGXFifoCommit(__PortGXPut_u32(__PortGXPut_u32(__PortGXPut_u8(p, 0x10), hdr), v));
            return;
        }
    }
    DdXFSlow(hdr, v, ra);
}

extern "C" void __PortGXDedupXF2(uint32_t addr, uint32_t a, uint32_t b, const void* ra) {
    if (!DdLive()) {
        DdRawXF2(addr, a, b);
        DdOutside(kDdXf);
        return;
    }
    const uint32_t w[2] = {a, b};
    const uint32_t tags[2] = {DdXfTag(addr), DdXfTag(addr + 1)};
    const bool same = PortGxDdXf(&sDd, addr, 2, w) != 0;
    DdLineNote(kDdXf, same, 13);
    if (__builtin_expect(sDdCensus != 0, 0)) DdNoteXf(addr, 2, w, tags, same, ra);
    if (same) {
        if (sDdMode == 1) return;
        if (sDdMode == 2) {
            DdProbe(2, addr, a);
            DdProbe(2, addr + 1, b);
        }
    }
    DdRawXF2(addr, a, b);
}

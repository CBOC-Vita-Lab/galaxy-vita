// Register file, command-stream parser and vertex decoding (front end).
#include "vgx_state.h"
#include "vgx_j3n2.h"
#include "vgx_twocore.h"
#include "vgx_guestmem.h"
#include "vgx_fixes.h"
#include "vgx_workload.h"
#include "vgx_fepar.h"
#include "vgx_deccen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx {

State g;

static GuestHooks g_hooks;
bool FixLodBias() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_LODBIAS", 0) != 0) != 0;
    return on;
}
int LodSamplerMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_LODBIAS_SAMPLER", 0);
        return (v == 1 || v == 2) && FixLodBias() ? v : 0L;
    }()));
    return m;
}
const lodsampler::Cfg& LodSamplerCfg() {
    static const lodsampler::Cfg c = [] { // (the switches the back end's BindTexture sums from, read the way it reads them)
        lodsampler::Cfg k;
        k.rungPct = std::clamp<long>(EnvInt("GXV_RUNG_LODBIAS", 0), 0, 200);
        k.rungExtra = std::clamp<long>(EnvInt("GXV_RUNG_LODBIAS_EXTRA", 0), 0, 24);
        if (const char* e = std::getenv("GXV_RUNG_LODBIAS_EXTRA_AF"); e && *e) k.rungExtra = std::max(k.rungExtra, std::clamp<long>(std::atol(e), 0, 24));
        k.playerHundredths = EnvInt("GXV_PLAYER_LODBIAS", 0);
        k.automipMode = static_cast<int>(EnvInt("GXV_AUTOMIP", 0));
        k.automipBias = static_cast<int>(std::clamp<long>(EnvInt("GXV_AUTOMIP_BIAS", -4), -16, 8));
        return k;
    }();
    return c;
}
void LodBiases(const uint32_t* bp, int out[8]) {
    for (unsigned m = 0; m < 8; ++m) {
        const unsigned r = fixes::TexMode0Reg(m);
        out[m] = static_cast<int>(std::lround(fixes::LodBiasOf(bp[r], bp[r + 4]) * 32.0f));
    }
}
bool FixTevIn() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_TEVIN", 0) != 0) != 0;
    return on;
}
bool TevOorTracked() {
    // GXV_FS_HALF=1 / 3 (vgx_shadergen.h FsHalfMode) also needs to know whether a TEV register is outside 0..255: its half precision analysis assumes
    // registers in 0..1. (Read here, not through FsHalfMode, so that this file does not depend on the shader generator.)
    // GXV_FS_TRIM (vgx_shadergen.h FsTrimMode) as well: its range analysis takes the registers as 0..1 unless the key is marked.
    const bool on = VGX_KNOB(FixTevIn() || [] { const long m = EnvInt("GXV_FS_HALF", 0); return m == 1 || m == 3; }() ||
                             EnvInt("GXV_FS_TRIM", 0) == 1) != 0;
    return on;
}
bool TevInCanonOn() {
    // (the switch of vgx_shadergen.h TevInCanonMode, read here so that this file does not depend on the shader generator)
    const bool on = VGX_KNOB([] { const long m = EnvInt("GXV_TEVIN_CANON", 0); return m == 1 || m == 2; }()) != 0;
    return on;
}
bool VsTrimTracked() {
    // (the switch of vgx_shadergen.h VsTrimMode, read here so that this file does not depend on the shader generator)
    const bool on = VGX_KNOB(EnvInt("GXV_VS_TRIM", 0) == 1) != 0;
    return on;
}
bool LtSkipOn() {
    // (the switch of vgx_shadergen.h VsLtSkipMode, read here so that this file does not depend on the shader generator)
    const bool on = VGX_KNOB(EnvInt("GXV_VS_LTSKIP", 0) == 1) != 0;
    return on;
}
void SetGuestHooks(const GuestHooks& h) { g_hooks = h; }
const GuestHooks& Hooks() { return g_hooks; }

namespace {
inline int sext11(uint32_t v) { return static_cast<int>(v << 21) >> 21; }

// GX attribute numbering (GXAttr)
enum { A_PNMTXIDX = 0, A_TEX0MTXIDX = 1, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_TEX7 = 20,
       A_POSMTXARRAY = 21, A_NBT = 25 };
enum { T_NONE = 0, T_DIRECT = 1, T_INDEX8 = 2, T_INDEX16 = 3 };
enum { C_U8 = 0, C_S8 = 1, C_U16 = 2, C_S16 = 3, C_F32 = 4 };
} // namespace

// ------------------------------------------------------------------ key-register hash (GXV_RSCACHE)
// State::keyHash: a hash of every register the shader keys are built from (BuildFsKey / BuildVsKey in vgx_draw.cpp: the BP
// registers MarkBP's first class lists, and XF 0x09, 0x0E..0x12, 0x40..0x47, 0x50..0x57), kept up to date as they change: each
// register contributes KeyZ(register, value), combined by exclusive or, so a change costs two evaluations and the hash says
// "the same keys as before" whatever order the registers were written in. vgx_draw.cpp's resolution cache looks the keys up by
// it instead of building and hashing them for every material change. (Off with GXV_RSCACHE=0: then it is not maintained.)
int g_rsCacheMode = -1;
int RsCacheModeSlow() {
    g_rsCacheMode = static_cast<int>(EnvInt("GXV_RSCACHE", 1));
    if (g_rsCacheMode == 2 && !diag::kEnabled) g_rsCacheMode = 1;
    if (g_rsCacheMode < 0 || g_rsCacheMode > 2) g_rsCacheMode = 1;
    // (GXV_RESOLVEFAST=0 or 2 are diagnostics of the old path, ResolveStateLegacy: asking for one takes that path unless
    // GXV_RSCACHE is set; tests/vgx_frontend_quiet.cpp counts the copies GXV_RESOLVEFAST=2 makes)
    if (g_rsCacheMode == 1 && EnvInt("GXV_RESOLVEFAST", 1) != 1 && EnvInt("GXV_RSCACHE", -1) == -1) g_rsCacheMode = 0;
    return g_rsCacheMode;
}
// (0x25 / 0x26, the indirect coordinate scales: GXV_IND_VS writes them into the vertex key, IndVsPair. A key register in every mode, so
// that the constant tables below stay constant; with the switch off they only add a key rebuild when a scale changes, and the TEV
// block is still marked: kMarkKey implies bpDirty.)
static constexpr bool IsKeyBP(uint32_t reg) {
    return reg == 0x00 || (reg >= 0x10 && reg <= 0x1F) || reg == 0x25 || reg == 0x26 || (reg >= 0x27 && reg <= 0x2F) || (reg >= 0x41 && reg <= 0x43) || (reg >= 0xC0 && reg <= 0xDF) ||
           reg == 0xF1 || reg == 0xF3 || (reg >= 0xF5 && reg <= 0xFD);
}
// GXV_FIX_LODBIAS: only effective bias presence feeds the shader key; its value feeds TEV uniforms, not the key.
static constexpr bool IsLodReg(uint32_t reg) { return (reg >= 0x80 && reg <= 0x87) || (reg >= 0xA0 && reg <= 0xA7); }
static constexpr bool IsKeyXF(uint32_t r) { return r == 0x09 || (r >= 0x0E && r <= 0x12) || (r >= 0x40 && r <= 0x47) || (r >= 0x50 && r <= 0x57); }
// KeyZ(i, v) for register i (BP 0..255, XF 0x100 + r): two 32-bit lanes, each a multiply-xorshift of the value keyed by a
// random word per register (the console's processor has no 64-bit multiply; the keys come from a fixed splitmix sequence).
struct KeyZTable {
    uint32_t a[0x15C], b[0x15C];
    constexpr KeyZTable() : a(), b() {
        uint64_t x = 0x243F6A8885A308D3ull;
        for (int i = 0; i < 0x15C; ++i) {
            for (int k = 0; k < 2; ++k) {
                x += 0x9E3779B97F4A7C15ull;
                uint64_t z = x;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                (k ? b : a)[i] = static_cast<uint32_t>(z) | 1u;
            }
        }
    }
};
constexpr KeyZTable kKeyZ;
static inline __attribute__((always_inline)) uint64_t KeyZ(uint32_t i, uint32_t v) {
    uint32_t a = (v ^ kKeyZ.a[i]) * 0x85EBCA6Bu;
    a ^= a >> 15; a *= 0xC2B2AE35u; a ^= a >> 13;
    uint32_t b = (v + kKeyZ.b[i]) * 0x846CA68Bu;
    b ^= b >> 16; b *= 0x2C1B3C6Du; b ^= b >> 12;
    return (static_cast<uint64_t>(a) << 32) | b;
}
static inline __attribute__((always_inline)) void KeyHashBP(uint32_t reg, uint32_t oldv, uint32_t newv) {
    if (RsCacheMode() != 0) g.keyHash ^= KeyZ(reg, oldv) ^ KeyZ(reg, newv);
}
static inline void KeyHashXF(uint32_t r, uint32_t oldv, uint32_t newv) {
    if (IsKeyXF(r) && RsCacheMode() != 0) g.keyHash ^= KeyZ(0x100u + r, oldv) ^ KeyZ(0x100u + r, newv);
}
uint64_t KeyHashOf(const State& s) {
    uint64_t h = 0;
    for (uint32_t r = 0; r < 256; ++r)
        if (IsKeyBP(r)) h ^= KeyZ(r, s.bp[r]);
    if (FixLodBias())
        for (unsigned m = 0; m < 8; ++m) {
            const unsigned r = fixes::TexMode0Reg(m);
            h ^= KeyZ(r, fixes::LodBiasOf(s.bp[r], s.bp[r + 4]) != 0.0f);
        }
    for (uint32_t r = 0; r < 0x58; ++r)
        if (IsKeyXF(r)) h ^= KeyZ(0x100u + r, s.xfReg[r]);
    if (LodSamplerMode() != 0) {
        int b[8];
        LodBiases(s.bp, b);
        const uint64_t d = lodsampler::PackBiases(b);
        h ^= KeyZ(0x15Au, static_cast<uint32_t>(d)) ^ KeyZ(0x15Bu, static_cast<uint32_t>(d >> 32));
    }
    if (TevOorTracked()) h ^= KeyZ(0x158u, s.tevOor);
    if (VsTrimTracked()) h ^= KeyZ(0x159u, s.ltCos1 | (static_cast<uint32_t>(s.ltDist1) << 8));
    return h;
}

// Which part of the resolved draw state a changed register invalidates. Most draws change
// only values (TEV colours, a matrix index, material colours); rebuilding and comparing the
// shader keys and the render state for each of those was a third of the player's time.
// Which part of a draw's resolved state a BP register feeds (ResolveState in vgx_draw.cpp;
// the lists are exactly the g.bp[] reads of BuildFsKey / BuildRenderState / BuildTev there:
// keep them in step). A register that none of them reads marks nothing: the texture-coordinate
// scale registers 0x30-0x3F, which change with every texture, used to cost a rebuild of
// both shader keys (145 of 202 rebuilds per open-play frame changed nothing).
// Fragment key (and with it render state and TEV block): genMode, indirect stages and
// their order, TEV order, blend mode, destination alpha, pixel format, TEV stages, fog
// type, alpha test, Z-texture mode, swap tables / konst selection: kMarkKey (bpKeyDirty, bpDirty).
// Render state only: scissor and its offset, line width, depth mode; and the Z-texture bias (0xF4), the depth a
// GX_ZT_REPLACE draw writes (BuildRenderState): kMarkRs (bpRsDirty).
// Values of the TEV uniform block only: TEV / konst colours, fog parameters and colour,
// Z-texture bias, indirect matrices and scales: kMarkTev (bpDirty).
enum : uint8_t { kMarkKey = 1, kMarkTev = 2, kMarkRs = 4 };
struct BpMarkTable {
    uint8_t t[256];
    constexpr BpMarkTable() : t() {
        for (int reg = 0; reg < 256; ++reg) {
            uint8_t c = 0;
            if (IsKeyBP(reg)) c = kMarkKey;
            else {
                if (reg == 0xF4 || reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x40 || reg == 0x59) c |= kMarkRs;
                if ((reg >= 0xE0 && reg <= 0xE7) || reg == 0xEE || reg == 0xEF || reg == 0xF0 || reg == 0xF2 || reg == 0xF4 || (reg >= 0x06 && reg <= 0x0E) ||
                    reg == 0x25 || reg == 0x26)
                    c |= kMarkTev;
            }
            t[reg] = c;
        }
    }
};
constexpr BpMarkTable kBpMark;
// Sampler-only changes still invalidate the binding at the caller, but do not rebuild unrelated shaders or uniforms.
static inline void LodRegChanged(uint32_t reg, uint32_t oldv, uint32_t newv) {
    if (!IsLodReg(reg) || !FixLodBias()) return;
    const unsigned mode0 = reg & ~4u;
    const float before = fixes::LodBiasOf(reg == mode0 ? oldv : g.bp[mode0], reg == mode0 ? g.bp[mode0 + 4] : oldv);
    const float after = fixes::LodBiasOf(reg == mode0 ? newv : g.bp[mode0], reg == mode0 ? g.bp[mode0 + 4] : newv);
    if (before == after) return;
    if (LodSamplerMode() != 0) { // (the sampler's decisions read every unit's bias: the hash and the key follow each change of a value, not only of its presence.
        // The word before the write is the current one with this unit's old bias: nothing is stored, so a register file copied or replayed wholesale (GXV_J3DNATIVE
        // checkpoints) cannot leave a stale copy behind: its hash holds the word of the registers it was made from.)
        int b[8];
        LodBiases(g.bp, b);
        const uint64_t d = lodsampler::PackBiases(b);
        b[(reg & 3) + (reg >= 0xA0 ? 4 : 0)] = static_cast<int>(std::lround(before * 32.0f));
        const uint64_t o = lodsampler::PackBiases(b);
        if (d != o) {
            if (RsCacheMode() != 0)
                g.keyHash ^= KeyZ(0x15Au, static_cast<uint32_t>(o)) ^ KeyZ(0x15Au, static_cast<uint32_t>(d)) ^ KeyZ(0x15Bu, static_cast<uint32_t>(o >> 32)) ^ KeyZ(0x15Bu, static_cast<uint32_t>(d >> 32));
            g.bpKeyDirty = true;
        }
    }
    if ((before != 0.0f) != (after != 0.0f)) {
        KeyHashBP(mode0, before != 0.0f, after != 0.0f);
        g.bpKeyDirty = true;
    }
    g.bpDirty = true;
}
static inline __attribute__((always_inline)) void MarkBP(uint8_t reg, uint32_t oldv, uint32_t newv) {
    const uint8_t c = kBpMark.t[reg];
    if (!c) return;
    if (c & kMarkKey) {
        KeyHashBP(reg, oldv, newv);
        g.bpKeyDirty = true;
    }
    if (c & (kMarkKey | kMarkTev)) g.bpDirty = true;
    if (c & kMarkRs) g.bpRsDirty = true;
    // (everything else: copy set-up 0x49-0x54, read when a copy is made; texture and TLUT
    // registers, bound through GXLoadTexObj; coordinate scales; performance counters)
}
static inline bool FogVtxTrack() {
    const bool on = VGX_KNOB(EnvInt("GXV_FOGVTX", 0) != 0) != 0; // (the same switch as FogVtxMode in vgx_shadergen.cpp; not called: more than one test links this file alone)
    return on;
}
static inline __attribute__((always_inline)) void MarkXFRegFlags(uint32_t r);
static inline __attribute__((always_inline)) void MarkXFReg(uint32_t r, uint32_t oldv, uint32_t newv) { // r = address - 0x1000
    KeyHashXF(r, oldv, newv);
    MarkXFRegFlags(r);
}
// (MarkXFReg's flags; GXV_SLDELTA keeps the key hash itself)
static inline __attribute__((always_inline)) void MarkXFRegFlags(uint32_t r) {
    if (r == 0x18 || r == 0x19) return;                   // matrix indices: xfMtxDirty (set by the caller)
    // GXV_FOGVTX: the vertex-fog coefficients in the TEV block come from the projection and the viewport's z range
    if (r - 0x1Cu <= 0xAu && FogVtxTrack()) g.bpDirty = true;
    if (r >= 0x1A && r <= 0x1F) g.xfViewDirty = true;
    else if (r >= 0x20 && r <= 0x26) g.xfProjDirty = true;
    else if (r >= 0x0A && r <= 0x0D) g.xfColorDirty = true;
    else if (r == 0x09 || (r >= 0x0E && r <= 0x12) || r == 0x3F || (r >= 0x40 && r <= 0x4F)) g.xfKeyDirty = true;
    else if (r >= 0x50 && r <= 0x57) { g.xfKeyDirty = true; g.xfMtxDirty = true; } // post-transform: key and matrix rows
    else g.xfRegDirty = true;
}

// ------------------------------------------------------------------ register no-op tests
// GXV_REGSHADOW (default 1): writes that repeat what a register holds are recognised as such where the register file alone could
// not tell: the TEV colour / konst halves at BP 0xE0..0xE7 (State::tevRaw) and the CP vertex descriptor and format registers
// (State::cpRaw), and the state-list replay handles those and the single XF register words inline (SlReplay kinds 6 and 7).
// 0: off (every such write takes the full path, as before); 2: the full path runs as well and must agree (logged).
int g_regShadow = -1;
int RegShadowSlow() {
    g_regShadow = static_cast<int>(EnvInt("GXV_REGSHADOW", 1));
    if (g_regShadow == 2 && !diag::kEnabled) g_regShadow = 1;
    return g_regShadow;
}
static inline int RegShadow() { return __builtin_expect(g_regShadow >= 0, 1) ? g_regShadow : RegShadowSlow(); }
static uint64_t g_shadowChecked = 0, g_shadowBad = 0;
void RegShadowCounters(uint64_t out[2]) { out[0] = g_shadowChecked; out[1] = g_shadowBad; }
static void ShadowCheckLog(bool ok, const char* what, uint32_t reg, uint32_t v) {
    ++g_shadowChecked;
    if (!ok) {
        ++g_shadowBad;
        VGX_LOG_N(16, "regshadow: MISMATCH (%s register 0x%02X, value 0x%08X)\n", what, reg, v);
    }
    if ((g_shadowChecked & 0xFFFFFu) == 0)
        VGX_LOG_N(200, "regshadow: %llu repeated writes checked, %llu mismatching\n", static_cast<unsigned long long>(g_shadowChecked),
                  static_cast<unsigned long long>(g_shadowBad));
}
// GXV_FIX_TEVIN (and GXV_FS_HALF=1 / 3): after a TEV colour register changed, keep State::tevOor (and the key hash) in step; a change marks the shader keys dirty.
// `idx`: the register just written. tevOor says whether any of the 16 components is outside 0..255 as of the last update (the only
// writers of tevReg, WriteTevColor and StoreBP, both come here), so only that register's four need looking at, unless it was the
// one outside and is inside now (GXV_SLDELTA: the scan of all sixteen was a fifth of a material list's replay with GXV_FIX_TEVIN).
static inline void TevOorUpdate(int idx) {
    if (!TevOorTracked()) return;
    const int* d = g.tevReg[idx];
    uint8_t oor = (static_cast<unsigned>(d[0]) > 255u || static_cast<unsigned>(d[1]) > 255u || static_cast<unsigned>(d[2]) > 255u ||
                   static_cast<unsigned>(d[3]) > 255u) ? 1 : 0;
    if (TevInCanonOn()) {
        // (GXV_TEVIN_CANON: tevOor is a mask, bit r = register r outside 0..255; only the register just written can change its bit)
        oor = static_cast<uint8_t>((g.tevOor & ~(1u << idx)) | (static_cast<unsigned>(oor) << idx));
    } else if (!oor && g.tevOor)
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 4; ++c)
                if (static_cast<unsigned>(g.tevReg[i][c]) > 255u) oor = 1;
    if (oor == g.tevOor) return;
    if (RsCacheMode() != 0) g.keyHash ^= KeyZ(0x158u, g.tevOor) ^ KeyZ(0x158u, oor);
    g.tevOor = oor;
    g.bpKeyDirty = true;
    g.bpDirty = true;
}
// TEV colour / konst write (BP 0xE0..0xE7, after the mask): the half of tevReg / tevKonst it names.
static inline void WriteTevColor(uint8_t reg, uint32_t nv) {
    const int idx = (reg - 0xE0) >> 1;
    int (*dst)[4] = (nv & 0x800000u) ? g.tevKonst : g.tevReg;
    const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
    int* const d = dst[idx];
    const uint8_t bit = static_cast<uint8_t>(1u << (idx + ((nv & 0x800000u) ? 4 : 0)));
    if (reg & 1) { if (d[2] != lo || d[1] != hi) { d[2] = lo; d[1] = hi; g.tevColDirty |= bit; } } // BG
    else { if (d[0] != lo || d[3] != hi) { d[0] = lo; d[3] = hi; g.tevColDirty |= bit; } }         // RA
    if (!(nv & 0x800000u)) TevOorUpdate(idx);
    g.bp[reg] = nv;
    g.tevRaw[((reg - 0xE0) << 1) | (nv >> 23)] = nv;
}
static inline uint32_t TevRawSlot(uint32_t reg, uint32_t nv) { return ((reg - 0xE0) << 1) | ((nv >> 23) & 1u); }
static void TevShadowCheck(uint8_t reg, uint32_t nv) {
    const int idx = (reg - 0xE0) >> 1;
    const int* d = ((nv & 0x800000u) ? g.tevKonst : g.tevReg)[idx];
    const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
    ShadowCheckLog((reg & 1) ? (d[2] == lo && d[1] == hi) : (d[0] == lo && d[3] == hi), "TEV colour", reg, nv);
}

namespace sc { void Marker(uint32_t kind, uint32_t payload); void ArrVerify(uint32_t kind, uint32_t payload); } // (GXV_STREAM_CENSUS, GXV_ARRAYSKIP=2: vgx_census.inc)
void GxDedupProbe(uint32_t a, uint32_t w); // (GXV_GX_DEDUP=2, port/gx_dedup.h: the game's probe, XF 0x0C00 / 0x0C01; below)
// GXV_HOTPROF_TEVCOL=2 (hotprof25; decomp/src/RVL_SDK/gx/GXTev.c): the game announces every TEV colour word that mode 1 would not write
// (the repeats of the BG word; in a particle loop a word equal to the loop's last for that register): markers BP 0xFF kind 28 (the
// register and the value's high byte) and kind 29 (the low 16 bits), right before the word itself. It must be what this register file
// holds: both the raw register and the colour half the bit-23 selector names. "tevcol verify:" every 2^18 checks and at each of the
// first 8 that differ (a differing one means something wrote the register behind the game's memory of it).
#if !defined(VGX_CLONE_B)
static uint32_t g_tvHi = 0;
static bool g_tvHave = false;
static uint64_t g_tvChecked = 0, g_tvBad = 0;
static void TevColVerify(uint32_t kind, uint32_t payload) {
    if (kind == 38u) { g_tvHi = payload; g_tvHave = true; return; }
    if (!g_tvHave) return;
    g_tvHave = false;
    const uint32_t reg = (g_tvHi >> 8) & 0xFFu, value = ((g_tvHi & 0xFFu) << 16) | payload;
    const bool ok = reg >= 0xE0 && reg <= 0xE7 && g.bpMask == 0x00FFFFFFu && g.bp[reg] == value && g.tevRaw[TevRawSlot(reg, value)] == value;
    ++g_tvChecked;
    if (!ok) ++g_tvBad;
    if ((!ok && g_tvBad <= 8) || (g_tvChecked & 0x3FFFFu) == 0) {
        if (port_log_summary)
            port_log_summary("tevcol verify: %llu checked, %llu differing%s (register %02X: front end %06X, the game would leave out %06X)",
                             static_cast<unsigned long long>(g_tvChecked), static_cast<unsigned long long>(g_tvBad), ok ? "" : ": MISMATCH", reg,
                             reg >= 0xE0 && reg <= 0xE7 ? g.bp[reg] : 0u, value);
    }
}
uint64_t TevColVerifyCounts(uint64_t* bad) { *bad = g_tvBad; return g_tvChecked; }
#else
static void TevColVerify(uint32_t, uint32_t) {}
#endif
// ------------------------------------------------------------------ BP
void WriteBP(uint32_t rv) {
    const uint8_t reg = static_cast<uint8_t>(rv >> 24);
    const uint32_t val = rv & 0x00FFFFFFu;
    if (reg == 0xFE) { // mask for the next write only
        g.bpMask = val;
        return;
    }
    const uint32_t nv = (g.bp[reg] & ~g.bpMask) | (val & g.bpMask);
    g.bpMask = 0x00FFFFFFu;

    if (reg >= 0xE0 && reg <= 0xE7) {
        // TEV colour registers and konst colours share addresses; bit 23 selects.
        const int shadow = RegShadow();
        if (shadow != 0 && g.tevRaw[TevRawSlot(reg, nv)] == nv) { // (this half holds that colour already)
            if (diag::kEnabled && shadow == 2) TevShadowCheck(reg, nv);
            g.bp[reg] = nv;
            return;
        }
        WriteTevColor(reg, nv);
        return;
    }
    switch (reg) {
    case 0x45: // PE done: everything before this command has been read from the game's memory
        g.bp[reg] = nv;
        if (nv & 2) fepar::SettleSources();   // (GXV_FE_PARALLEL: the decodes still to be done read the game's memory)
        if ((nv & 2) && g_hooks.peFinish) g_hooks.peFinish();
        return;
    case 0x47: case 0x48: // draw-sync token (0x47: with interrupt)
        g.bp[reg] = nv;
        fepar::SettleSources();
        if (g_hooks.peToken) g_hooks.peToken(static_cast<uint16_t>(nv), reg == 0x47);
        return;
    case 0x52: // EFB copy trigger: the set-up registers 0x49-0x51 are already in g.bp
        g.bp[reg] = nv;
        BpCopyTrigger(nv);
        return;
    case 0x65: // palette load: 0x64 holds the source address
        g.bp[reg] = nv;
        BpTlutLoad();
        return;
    case 0xFF: // Port-only ordered metadata (presentation eligibility or PORT_GX_MARK_*).
               // Never stored or deduplicated: every marker must reach the pending buffer.
        {
            const uint32_t kind = val & 0xFFu;
            if (kind == kPresentAllowedMarker || kind == kPresentSuppressedMarker) PresentationMarker(kind);
            else if (kind == kPresentStampMarker) PresentationStamp((val >> 8) & 0xFFFFu);
            else if (kind == 10) TransientMarker((val >> 8) & 0xFFFFu); // (PORT_GX_MARK_TRANSIENT_COPY + the stage, GXV_INDCAP_SKIP)
            else if (hrwpair::IsMarker(kind)) {
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                HrwMarker(hrwpair::Token(val));
            }
            else if (kind == kOwnerMarker) {                              // (PORT_GX_MARK_OWNER + the key, GXV_HRW: vgx_draw.cpp OwnerMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                OwnerMarker((val >> 8) & 0xFFFFu);
            }
            else if (kind == kFreshEndMarker) {                           // (PORT_GX_MARK_FRESH_END + the pair's number, GXV_HRW: vgx_draw.cpp FreshEndMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                FreshEndMarker((val >> 8) & 0xFFFFu);
            }
            else if (kind == 32) {                                        // (PORT_GX_MARK_SILH_HINT, GXV_SILH_GATE: vgx_draw.cpp SilhHintMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                SilhHintMarker((val >> 8) & 0xFFu);
            }
            else if (kind == 7) SceneDepthMarker();                      // (hrw41: PORT_GX_MARK_SCENE_DEPTH, written only with the depth mask's switches: vgx_draw.cpp)
            else if (kind == 11) GeoPhaseMarker((val >> 8) & 0xFFu);    // (PORT_GX_MARK_PHASE, GXV_GEOPASS)
            else if (kind == 14 || kind == 15) {                         // (PORT_GX_MARK_WARM_BEGIN / _END, GXV_PREWARM: vgx_draw.cpp WarmScopeMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1); // (GXV_FE_TWOCORE: the warm-up scope is the player's: the second worker gives the picture up)
#endif
                WarmScopeMarker(kind);
            }
            else if (kind >= 24 && kind <= 27) {                         // (GXV_ARRAYSKIP=2, port/array_skip.h: a write the game would leave out, checked here)
                sc::ArrVerify(kind, (val >> 8) & 0xFFFFu);
            }
            else if (kind == 38 || kind == 39) TevColVerify(kind, (val >> 8) & 0xFFFFu); // (GXV_HOTPROF_TEVCOL=2: above)
            else if (kind == 40) {                                       // (PORT_GX_MARK_BLOOM_BRACKET + the sub-kind, GXV_BLOOM_BRACKET: vgx_draw.cpp BloomBracketMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                BloomBracketMarker((val >> 8) & 0xFFu);
            }
            else if (kind >= 21 && kind <= 23) {                         // (PORT_GX_MARK_CENSUS*, GXV_STREAM_CENSUS: vgx_census.inc; written only in a counted picture)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                sc::Marker(kind, (val >> 8) & 0xFFFFu);
            }
            else if (kind >= 28 && kind <= 31) {                         // (PORT_GX_MARK_BGCAT / BG_BEGIN / BG_END / BG_REUSE, GXV_BGLAYER: vgx_bglayer.inc)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                BgMarker(kind, (val >> 8) & 0xFFFFu);
            }
            else if (kind == 33) {                                       // (PORT_GX_MARK_STAGE_ENUM_DRAW, GXV_STAGE_ENUM: vgx_draw.cpp StageEnumDrawMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                StageEnumDrawMarker((val >> 8) & 0xFFFFu);
            }
            else if (kind == 19 || kind == 20) {                         // (PORT_GX_MARK_LAYOUT_BEGIN / _END, GXV_LAYOUTWARM_LOG: vgx_draw.cpp LayoutCensusMarker)
#if defined(VGX_CLONE_B)
                two::Abandon(1);
#endif
                LayoutCensusMarker(kind, (val >> 8) & 0xFFFFu);
            }
            else CharaMarker(kind); // (includes PORT_GX_MARK_PLAYER_BEGIN/END 5/6: tracks g_playerMarkDepth identically to HUD 1/2; vgx2_b_draw_can_begin refuses segments while > 0)
        }
        return;
    default:
        if (reg >= 0x80 && reg <= 0xBB) {
            // Texture unit registers (mode0, mode1, image0, image3, tlut; image1 / image2 describe
            // the texture cache and mean nothing here). The game's GX library repeats them for
            // every material: only a change makes the unit's binding stale.
            if (g.bp[reg] != nv) {
                const uint32_t ov = g.bp[reg];
                g.bp[reg] = nv;
                LodRegChanged(reg, ov, nv);
                const int base = reg >= 0xA0 ? 0xA0 : 0x80;
                const int kind = (reg - base) >> 2; // mode0 mode1 image0 image1 image2 image3 tlut
                if (kind != 3 && kind != 4 && kind != 7) {
                    const int map = (reg & 3) + (base == 0xA0 ? 4 : 0);
                    g.texDirty = true;
                    g.texSlotDirty |= static_cast<uint8_t>(1u << map);
                    g.bpTexStale |= static_cast<uint8_t>(1u << map);
                }
            }
            return;
        }
        break;
    }
    if (g.bp[reg] != nv) {
        const uint32_t ov = g.bp[reg];
        g.bp[reg] = nv;
        MarkBP(reg, ov, nv);
    }
}

void StoreBP(uint8_t reg, uint32_t nv) {
    if (reg >= 0xE0 && reg <= 0xE7) {
        const int idx = (reg - 0xE0) >> 1;
        int (*dst)[4] = (nv & 0x800000u) ? g.tevKonst : g.tevReg;
        const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
        int old[4];
        std::memcpy(old, dst[idx], sizeof old);
        if (reg & 1) { dst[idx][2] = lo; dst[idx][1] = hi; } // BG
        else { dst[idx][0] = lo; dst[idx][3] = hi; }         // RA
        if (std::memcmp(old, dst[idx], sizeof old) != 0) g.tevColDirty |= static_cast<uint8_t>(1u << (idx + ((nv & 0x800000u) ? 4 : 0)));
        if (!(nv & 0x800000u)) TevOorUpdate(idx);
        g.bp[reg] = nv;
        g.tevRaw[TevRawSlot(reg, nv)] = nv & 0x00FFFFFFu;
        return;
    }
    if (g.bp[reg] != nv) {
        const uint32_t ov = g.bp[reg];
        g.bp[reg] = nv;
        MarkBP(reg, ov, nv);
    }
}

void SetBPField(uint8_t reg, int shift, int count, uint32_t value) {
    const uint32_t mask = ((1u << count) - 1u) << shift;
    const uint32_t nv = (g.bp[reg] & ~mask) | ((value << shift) & mask);
    if (nv != g.bp[reg]) {
        const uint32_t ov = g.bp[reg];
        g.bp[reg] = nv & 0x00FFFFFFu;
        MarkBP(reg, ov, g.bp[reg]);
    }
}

// ------------------------------------------------------------------ XF
// GXV_VS_TRIM: after an attenuation word of light l changed, its two facts (VsKey::ltCos1 / ltDist1: a term exactly (1, 0, 0); +0 and -0
// both count as zero) again; a change of either updates the key hash and marks the vertex key dirty (the only writer of xfLight is
// WriteXFWord, so the facts always describe it).
static void LightFactsUpdate(uint32_t l) {
    const uint32_t* L = g.xfLight[l];
    const uint32_t kOne = 0x3F800000u, bit = 1u << l;
    const bool c1 = L[4] == kOne && ((L[5] | L[6]) << 1) == 0, d1 = L[7] == kOne && ((L[8] | L[9]) << 1) == 0;
    const uint8_t nc = static_cast<uint8_t>(c1 ? (g.ltCos1 | bit) : (g.ltCos1 & ~bit)), nd = static_cast<uint8_t>(d1 ? (g.ltDist1 | bit) : (g.ltDist1 & ~bit));
    if (nc == g.ltCos1 && nd == g.ltDist1) return;
    if (RsCacheMode() != 0)
        g.keyHash ^= KeyZ(0x159u, g.ltCos1 | (static_cast<uint32_t>(g.ltDist1) << 8)) ^ KeyZ(0x159u, nc | (static_cast<uint32_t>(nd) << 8));
    g.ltCos1 = nc;
    g.ltDist1 = nd;
    g.xfKeyDirty = true;
}
static void WriteXFWord(uint32_t a, uint32_t w) {
    // (the register block first: the single words the game sends, matrix indices, texture-coordinate set-up, projection, are
    // nearly all there, and the ranges do not overlap, so the order does not matter)
    if (a >= 0x1000) {
        if (a < 0x1058) {
            uint32_t& d = g.xfReg[a - 0x1000];
            if (a == 0x1018) { if (g.matIdxA != w) { g.matIdxA = w; g.xfMtxDirty = true; } }
            else if (a == 0x1019) { if (g.matIdxB != w) { g.matIdxB = w; g.xfMtxDirty = true; } }
            if (d != w) { const uint32_t o = d; d = w; MarkXFReg(a - 0x1000, o, w); }
        }
    } else if (a < 0x100) {
        float& f = g.xfPos[a >> 2][a & 3];
        if (f2u(f) != w) { f = u2f(w); g.xfPosRowsDirty |= 1ull << (a >> 2); }
    } else if (a >= 0x400 && a < 0x460) {
        const uint32_t o = a - 0x400;
        float& f = g.xfNrm[o / 3][o % 3];
        if (f2u(f) != w) { f = u2f(w); g.xfMtxDirty = true; }
    } else if (a >= 0x500 && a < 0x600) {
        const uint32_t o = a - 0x500;
        float& f = g.xfPost[o >> 2][o & 3];
        if (f2u(f) != w) { f = u2f(w); g.xfMtxDirty = true; }
    } else if (a >= 0x600 && a < 0x680) {
        const uint32_t o = a - 0x600;
        uint32_t& d = g.xfLight[o >> 4][o & 15];
        if (d != w) {
            d = w;
            g.xfLightDirty = true;
            if ((o & 15) >= 4 && (o & 15) <= 9 && VsTrimTracked()) LightFactsUpdate(o >> 4);
        }
    } else if ((a & ~1u) == 0x0C00u) {
        GxDedupProbe(a, w); // (GXV_GX_DEDUP=2: XF memory the console does not have; nothing is kept here)
    }
}
void WriteXF(uint32_t addr, uint32_t count, const uint32_t* w) {
    // Whole matrices are the common case (12 words of position / texture
    // matrix memory, 9 of normal matrix memory): one compare and one copy
    // instead of a classification per word.
    if (count > 1) {
        if (addr < 0x100 && addr + count <= 0x100) {
            float* dst = &g.xfPos[0][0] + addr; // rows of 4 are contiguous in address order
            if (!SameWords(dst, w, count)) {
                CopyWords(dst, w, count);
                const uint32_t r0 = addr >> 2, r1 = (addr + count - 1) >> 2;
                g.xfPosRowsDirty |= (r1 - r0 >= 63 ? ~0ull : ((1ull << (r1 - r0 + 1)) - 1ull) << r0);
            }
            return;
        }
        // Registers written again with the values they hold (the game sets the projection
        // before every particle): nothing to classify.
        if (addr >= 0x1000 && addr + count <= 0x1058 && !(addr <= 0x1019 && addr + count > 0x1018)) {
            if (SameWords(&g.xfReg[addr - 0x1000], w, count)) return;
            if (RegShadow() == 1) { // (WriteXFWord's register path for each word, inline: no matrix index among them)
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t& d = g.xfReg[addr - 0x1000 + i];
                    if (d != w[i]) { const uint32_t o = d; d = w[i]; MarkXFReg(addr - 0x1000 + i, o, w[i]); }
                }
                return;
            }
        }
        if (addr >= 0x400 && addr + count <= 0x460) {
            float* dst = &g.xfNrm[0][0] + (addr - 0x400);
            if (!SameWords(dst, w, count)) { CopyWords(dst, w, count); g.xfMtxDirty = true; }
            return;
        }
        if (addr >= 0x500 && addr + count <= 0x600) {
            float* dst = &g.xfPost[0][0] + (addr - 0x500);
            if (!SameWords(dst, w, count)) { CopyWords(dst, w, count); g.xfMtxDirty = true; }
            return;
        }
    }
    for (uint32_t i = 0; i < count; ++i) WriteXFWord(addr + i, w[i]);
}
// Big-endian words into matrix memory in one pass (byte swap, compare, store), as WriteXF's matrix-memory paths leave it:
// true if any word changed. (The matrix loads of the command stream, ~1,800 a picture in the Comet Observatory, went through
// a swapped copy on the stack, a compare and a copy.)
static inline bool SwapStoreDiff(float* dstf, const uint8_t* be, uint32_t n) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dstf);
    uint32_t diff = 0;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    uint32x4_t acc = vdupq_n_u32(0);
    for (; n >= 4; n -= 4, d += 16, be += 16) {
        const uint8x16_t v = vrev32q_u8(vld1q_u8(be));
        acc = vorrq_u32(acc, vreinterpretq_u32_u8(veorq_u8(v, vld1q_u8(d))));
        vst1q_u8(d, v);
    }
    const uint32x2_t h = vorr_u32(vget_low_u32(acc), vget_high_u32(acc));
    diff = vget_lane_u32(h, 0) | vget_lane_u32(h, 1);
#endif
    for (; n > 0; --n, d += 4, be += 4) {
        const uint32_t w = be32(be);
        uint32_t o;
        std::memcpy(&o, d, 4);
        diff |= o ^ w;
        std::memcpy(d, &w, 4);
    }
    return diff != 0;
}
void WriteXFBE(uint32_t addr, uint32_t count, const uint8_t* be) {
    if (count > 1 && count <= 16 && RegShadow() == 1) { // (GXV_REGSHADOW: the matrix memories in one pass)
        if (addr < 0x100 && addr + count <= 0x100) {
            if (SwapStoreDiff(&g.xfPos[0][0] + addr, be, count)) {
                const uint32_t r0 = addr >> 2, r1 = (addr + count - 1) >> 2;
                g.xfPosRowsDirty |= (r1 - r0 >= 63 ? ~0ull : ((1ull << (r1 - r0 + 1)) - 1ull) << r0);
            }
            return;
        }
        if (addr >= 0x400 && addr + count <= 0x460) {
            if (SwapStoreDiff(&g.xfNrm[0][0] + (addr - 0x400), be, count)) g.xfMtxDirty = true;
            return;
        }
        if (addr >= 0x500 && addr + count <= 0x600) {
            if (SwapStoreDiff(&g.xfPost[0][0] + (addr - 0x500), be, count)) g.xfMtxDirty = true;
            return;
        }
    }
    if (diag::kEnabled && count > 1 && count <= 16 && RegShadow() == 2 &&
        (addr + count <= 0x100 || (addr >= 0x400 && addr + count <= 0x460) || (addr >= 0x500 && addr + count <= 0x600))) {
        // (verify: the one-pass store on a copy of the words, against the path below)
        float* dst = addr < 0x100 ? &g.xfPos[0][0] + addr : addr < 0x500 ? &g.xfNrm[0][0] + (addr - 0x400) : &g.xfPost[0][0] + (addr - 0x500);
        float before[16], fast[16];
        std::memcpy(before, dst, count * 4);
        std::memcpy(fast, dst, count * 4);
        const bool changed = SwapStoreDiff(fast, be, count);
        uint32_t w[16];
        for (uint32_t i = 0; i < count; ++i) w[i] = be32(be + i * 4);
        WriteXF(addr, count, w);
        ShadowCheckLog(std::memcmp(fast, dst, count * 4) == 0 && changed == (std::memcmp(before, dst, count * 4) != 0), "XF matrix", addr, count);
        return;
    }
    if (count > 1 && count <= 16) { // (a command carries at most 16 words)
        uint32_t w[16];
        for (uint32_t i = 0; i < count; ++i) w[i] = be32(be + i * 4);
        WriteXF(addr, count, w);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) WriteXFWord(addr + i, be32(be + i * 4));
}

// ------------------------------------------------------------------ CP
void SetVcd(int attr, int type) {
    // GX_VA_NBT is not a separate vertex component: the runtime's HLE folds it
    // into GX_VA_NRM (descriptor) and the normal count (format) and republishes
    // both before every draw, so the NBT pseudo-attribute itself is ignored.
    if (attr < 0 || attr > A_TEX7) return;
    if (g.vcd[attr] != type) {
        g.vcd[attr] = static_cast<uint8_t>(type);
        g.layoutDirty = true;
        ++g.vcdGen;
    }
}
void SetVat(int fmt, int attr, int cnt, int type, int frac) {
    if (fmt < 0 || fmt > 7 || attr < A_POS || attr > A_TEX7) return;
    VtxAttrFmt& f = g.vat[fmt][attr];
    if (f.cnt != cnt || f.type != type || f.frac != frac) {
        f.cnt = static_cast<uint8_t>(cnt);
        f.type = static_cast<uint8_t>(type);
        f.frac = static_cast<uint8_t>(frac);
        g.layoutDirty = true;
    }
}
// The slot of State::cpRaw a CP register has (-1: none): 0x50, 0x60, 0x70..0x77, 0x80..0x87, 0x90..0x97.
static inline int CpRawSlot(uint8_t reg) {
    if (reg == 0x50) return 0;
    if (reg == 0x60) return 1;
    if (reg >= 0x70 && reg <= 0x97 && (reg & 0xF) < 8) return 2 + ((reg >> 4) - 7) * 8 + (reg & 7);
    return -1;
}
static void WriteCPFull(uint8_t reg, uint32_t v);
void WriteCP(uint8_t reg, uint32_t v) {
    if (reg >= 0xA0 && reg <= 0xBF) { // (the commonest: array bases and strides, as in WriteCPFull)
        const int n = reg & 0xF;
        uint32_t& d = (reg & 0x10) ? g.cpStride[n] : g.cpBase[n];
        const uint32_t nv = (reg & 0x10) ? (v & 0xFF) : v;
        if (d != nv) { d = nv; g.cpArrayStale |= static_cast<uint16_t>(1u << n); }
        return;
    }
    if (reg >= 0x50 && reg < 0xA0) {
        const int slot = CpRawSlot(reg);
        if (slot >= 0) {
            const int shadow = RegShadow();
            const uint32_t bit = 1u << slot;
            if (shadow != 0 && (g.cpRawValid & bit) && g.cpRaw[slot] == v) {
                if (diag::kEnabled && shadow == 2) {
                    // (the full path must change nothing)
                    uint8_t vcd[26];
                    VtxAttrFmt vat[8][26];
                    std::memcpy(vcd, g.vcd, sizeof vcd);
                    std::memcpy(vat, g.vat, sizeof vat);
                    const uint32_t gen = g.vcdGen;
                    const bool ld = g.layoutDirty;
                    WriteCPFull(reg, v);
                    ShadowCheckLog(std::memcmp(vcd, g.vcd, sizeof vcd) == 0 && std::memcmp(vat, g.vat, sizeof vat) == 0 && gen == g.vcdGen && ld == g.layoutDirty,
                                   "CP", reg, v);
                }
                return;
            }
            g.cpRaw[slot] = v;
            g.cpRawValid |= bit;
        }
    }
    WriteCPFull(reg, v);
}
static void WriteCPFull(uint8_t reg, uint32_t v) {
    // 0xA0..0xAF: array base (console address), 0xB0..0xBF: stride. Arrays 0-11 are POS, NRM, CLR0, CLR1, TEX0-7; 12-15 the
    // matrix / light arrays of the indexed XF loads. J3D writes the three bases of every shape it draws, mostly with the values
    // they already hold (the host pointer is looked up when a draw, or an indexed load, first needs the array): the commonest
    // case, so first.
    if (reg >= 0xA0 && reg <= 0xBF) {
        const int n = reg & 0xF;
        uint32_t& d = (reg & 0x10) ? g.cpStride[n] : g.cpBase[n];
        const uint32_t nv = (reg & 0x10) ? (v & 0xFF) : v;
        if (d != nv) { d = nv; g.cpArrayStale |= static_cast<uint16_t>(1u << n); }
        return;
    }
    if (reg == 0x30) { if (g.matIdxA != v) { g.matIdxA = v; g.xfMtxDirty = true; } return; }
    if (reg == 0x40) { if (g.matIdxB != v) { g.matIdxB = v; g.xfMtxDirty = true; } return; }
    if (reg == 0x50) {
        for (int i = 0; i <= 8; ++i) SetVcd(i, (v >> i) & 1);
        SetVcd(A_POS, (v >> 9) & 3);
        SetVcd(A_NRM, (v >> 11) & 3);
        SetVcd(A_CLR0, (v >> 13) & 3);
        SetVcd(A_CLR1, (v >> 15) & 3);
        return;
    }
    if (reg == 0x60) {
        for (int t = 0; t < 8; ++t) SetVcd(A_TEX0 + t, (v >> (t * 2)) & 3);
        return;
    }
    const int fmt = reg & 7;
    if (reg >= 0x70 && reg <= 0x77) {
        SetVat(fmt, A_POS, v & 1, (v >> 1) & 7, (v >> 4) & 0x1F);
        SetVat(fmt, A_NRM, (v & 0x80000000u) ? 2 : ((v >> 9) & 1), (v >> 10) & 7, 0);
        SetVat(fmt, A_CLR0, (v >> 13) & 1, (v >> 14) & 7, 0);
        SetVat(fmt, A_CLR1, (v >> 17) & 1, (v >> 18) & 7, 0);
        SetVat(fmt, A_TEX0, (v >> 21) & 1, (v >> 22) & 7, (v >> 25) & 0x1F);
        return;
    }
    if (reg >= 0x80 && reg <= 0x87) {
        for (int t = 1; t <= 3; ++t) {
            const int s = (t - 1) * 9;
            SetVat(fmt, A_TEX0 + t, (v >> s) & 1, (v >> (s + 1)) & 7, (v >> (s + 4)) & 0x1F);
        }
        SetVat(fmt, A_TEX0 + 4, (v >> 27) & 1, (v >> 28) & 7, g.vat[fmt][A_TEX0 + 4].frac);
        return;
    }
    if (reg >= 0x90 && reg <= 0x97) {
        const VtxAttrFmt t4 = g.vat[fmt][A_TEX0 + 4];
        SetVat(fmt, A_TEX0 + 4, t4.cnt, t4.type, v & 0x1F);
        for (int t = 5; t <= 7; ++t) {
            const int s = 5 + (t - 5) * 9;
            SetVat(fmt, A_TEX0 + t, (v >> s) & 1, (v >> (s + 1)) & 7, (v >> (s + 4)) & 0x1F);
        }
        return;
    }
}

void BpArraySync(int n) {
    g.cpArrayStale &= static_cast<uint16_t>(~(1u << n));
    ArrayRef& a = g.arrays[n < 12 ? A_POS + n : A_POSMTXARRAY + (n - 12)];
    uint32_t avail = 0;
    const uint8_t* p = g_hooks.resolve ? g_hooks.resolve(g.cpBase[n], &avail) : nullptr;
    a.data = p;
    a.size = p ? avail : 0;
    a.stride = g.cpStride[n];
    a.le = true; // vertex arrays are host data in this port: cooked model data, or written by the game's code
}
static inline void SyncArrays(uint16_t mask) {
    uint16_t m = g.cpArrayStale & mask;
    while (m) { const int n = __builtin_ctz(m); m &= static_cast<uint16_t>(m - 1); BpArraySync(n); }
}

void InitState(State& g) {
    g = State{};
    g.bpMask = 0x00FFFFFFu;
    g.bp[0x00] = 0; // genMode: one TEV stage, no texgens
    g.bp[0xF3] = 0x3F0000; // alpha compare: ALWAYS / ALWAYS
    for (int f = 0; f < 8; ++f)
        for (int a = 0; a < 26; ++a) g.vat[f][a] = VtxAttrFmt{};
    g.vcd[A_POS] = T_DIRECT;
    // Identity in the matrix slots the SDK defaults to.
    for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 4; ++c) g.xfPos[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 4; ++c) g.xfPost[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    for (int r = 0; r < 32; ++r)
        for (int c = 0; c < 3; ++c) g.xfNrm[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    g.matIdxA = 0x3CF3CF00u;
    g.matIdxB = 0x00F3CF3Cu;
    for (int i = 0; i < 8; ++i) g.xfReg[0x50 + i] = 61; // post matrix = identity slot
    g.xfReg[0x1A] = f2u(320.0f); g.xfReg[0x1B] = f2u(-240.0f); g.xfReg[0x1C] = f2u(16777215.0f);
    g.xfReg[0x1D] = f2u(320.0f + 342.0f); g.xfReg[0x1E] = f2u(240.0f + 342.0f); g.xfReg[0x1F] = f2u(16777215.0f);
    g.xfReg[0x26] = 1;
    g.xfReg[0x20] = f2u(1.0f); g.xfReg[0x22] = f2u(1.0f); g.xfReg[0x24] = f2u(1.0f);
    g.keyHash = KeyHashOf(g);
}
void ResetState() { J3n2Flush(); InitState(g); }

// ------------------------------------------------------- vertex decoding
namespace {

PlanCache g_plans;

inline int CompSize(int type) { return type == C_F32 ? 4 : (type >= C_U16 ? 2 : 1); }
inline int ColorSize(int type) {
    switch (type) {
    case 0: return 2; // RGB565
    case 1: return 3; // RGB8
    case 2: return 4; // RGBX8
    case 3: return 2; // RGBA4
    case 4: return 3; // RGBA6
    default: return 4; // RGBA8
    }
}
inline int NrmFrac(int type) {
    switch (type) { case C_U8: return 7; case C_S8: return 6; case C_U16: return 15; case C_S16: return 14; default: return 0; }
}

} // namespace

static void FinishDirectPlan(Plan& p); // (after the decode loops)
const Plan& GetPlanIn(PlanCache& cache, bool& dirty, const uint8_t* vcd, const VtxAttrFmt (*vat)[26], int fmt, bool recordStats) {
    if (dirty) {
        for (auto& p : cache.p) p.valid = false;
        for (auto& f : cache.f) f.valid = false;
        dirty = false;
    }
    Plan& p = cache.p[fmt & 7];
    if (p.valid) return p;
    cache.f[fmt & 7].valid = false;
    p = Plan{};
    uint32_t mask = 0;
    if (vcd[A_NRM]) mask |= VL_NRM;
    if (vcd[A_CLR0]) mask |= VL_COL0;
    if (vcd[A_CLR1]) mask |= VL_COL1;
    for (int t = 0; t < 8; ++t) {
        if (vcd[A_TEX0 + t]) mask |= VL_TEX0 << t;
        if (vcd[A_TEX0MTXIDX + t]) mask |= VL_TMI;
    }
    p.layoutMask = mask;
    p.layout = MakeLayout(mask);
    const VtxLayout& L = p.layout;
    for (int attr = 0; attr <= A_TEX7; ++attr) {
        const int mode = vcd[attr];
        if (mode == T_NONE) continue;
        AttrPlan ap{};
        ap.attr = static_cast<uint8_t>(attr);
        ap.mode = static_cast<uint8_t>(mode);
        const VtxAttrFmt& f = vat[fmt & 7][attr];
        ap.type = f.type; ap.cnt = f.cnt; ap.frac = f.frac;
        int direct = 0;
        if (attr <= 8) {
            ap.mode = T_DIRECT;
            direct = 1;
            ap.outOff = attr == 0 ? 12 : static_cast<uint8_t>(L.offTmi + (attr - 1));
            p.callsPerVertex += 1;
        } else if (attr == A_POS) {
            ap.comps = f.cnt ? 3 : 2;
            direct = ap.comps * CompSize(f.type);
            ap.outOff = 0;
            p.callsPerVertex += 1;
        } else if (attr == A_NRM) {
            ap.comps = f.cnt ? 9 : 3;
            ap.frac = static_cast<uint8_t>(NrmFrac(f.type));
            direct = ap.comps * CompSize(f.type);
            ap.outOff = L.offNrm;
            p.callsPerVertex += f.cnt ? 3 : 1;
        } else if (attr <= A_CLR1) {
            direct = ColorSize(f.type);
            ap.outOff = attr == A_CLR0 ? L.offCol0 : L.offCol1;
            p.callsPerVertex += 1;
        } else {
            ap.comps = f.cnt ? 2 : 1;
            direct = ap.comps * CompSize(f.type);
            ap.outOff = L.offTex[attr - A_TEX0];
            p.callsPerVertex += 1;
        }
        ap.elemSize = static_cast<uint8_t>(direct);
        if (ap.mode == T_DIRECT) ap.srcSize = static_cast<uint8_t>(direct);
        else {
            const int idx = ap.mode == T_INDEX8 ? 1 : 2;
            // NBT3: three indices, each selecting one 3-component vector.
            ap.srcSize = static_cast<uint8_t>((attr == A_NRM && f.cnt == 2) ? idx * 3 : idx);
        }
        ap.srcOff = static_cast<uint16_t>(p.streamSize);
        p.streamSize += ap.srcSize;
        p.a[p.n++] = ap;
        if (recordStats && stats::Detailed()) stats::VertexFormat(attr, mode, f); // (a std::set<std::string> insert: only for the GXV_STATS report)
    }
    FinishDirectPlan(p);
    p.valid = true;
    return p;
}

// Where an attribute of a vertex of layout L is stored (as GetPlanIn sets AttrPlan::outOff).
static uint8_t OutOffFor(int attr, const VtxLayout& L) {
    if (attr == 0) return 12;
    if (attr <= 8) return static_cast<uint8_t>(L.offTmi + (attr - 1));
    if (attr == A_POS) return 0;
    if (attr == A_NRM) return L.offNrm;
    if (attr == A_CLR0) return L.offCol0;
    if (attr == A_CLR1) return L.offCol1;
    return L.offTex[attr - A_TEX0];
}
const Plan& GetFoldedPlanIn(PlanCache& cache, int fmt, const Plan& base) {
    Plan& f = cache.f[fmt & 7];
    if (f.valid) return f;
    f = base;
    f.layoutMask = base.layoutMask | VL_COL0 | VL_COL1;
    f.layout = MakeLayout(f.layoutMask);
    for (int i = 0; i < f.n; ++i) f.a[i].outOff = OutOffFor(f.a[i].attr, f.layout);
    f.valid = true;
    return f;
}

namespace {
inline const Plan& GetPlan(int fmt) { return GetPlanIn(g_plans, g.layoutDirty, g.vcd, g.vat, fmt, true); }

inline float ReadComp(const uint8_t* s, int type, bool le) {
    switch (type) {
    case C_U8: return static_cast<float>(s[0]);
    case C_S8: return static_cast<float>(static_cast<int8_t>(s[0]));
    case C_U16: return static_cast<float>(le ? static_cast<uint16_t>(s[0] | (s[1] << 8)) : be16(s));
    case C_S16: return static_cast<float>(static_cast<int16_t>(le ? static_cast<uint16_t>(s[0] | (s[1] << 8)) : be16(s)));
    default:
        if (le) { float f; std::memcpy(&f, s, 4); return f; }
        return bef32(s);
    }
}
inline uint16_t rd16(const uint8_t* s, bool le) { return le ? static_cast<uint16_t>(s[0] | (s[1] << 8)) : be16(s); }
inline uint32_t rd32(const uint8_t* s, bool le) {
    if (le) { uint32_t v; std::memcpy(&v, s, 4); return v; }
    return be32(s);
}
inline void DecodeColor(const uint8_t* s, int type, uint8_t* o, bool le = false) {
    switch (type) {
    case 0: { // RGB565
        const uint16_t v = rd16(s, le);
        const int r = (v >> 11) & 0x1F, gg = (v >> 5) & 0x3F, b = v & 0x1F;
        o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((gg << 2) | (gg >> 4));
        o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
        break;
    }
    case 1: case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
    case 3: { // RGBA4
        const uint16_t v = rd16(s, le);
        o[0] = static_cast<uint8_t>(((v >> 12) & 0xF) * 17); o[1] = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
        o[2] = static_cast<uint8_t>(((v >> 4) & 0xF) * 17); o[3] = static_cast<uint8_t>((v & 0xF) * 17);
        break;
    }
    case 4: { // RGBA6
        const uint32_t v = (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
        const int r = (v >> 18) & 0x3F, gg = (v >> 12) & 0x3F, b = (v >> 6) & 0x3F, a = v & 0x3F;
        o[0] = static_cast<uint8_t>((r << 2) | (r >> 4)); o[1] = static_cast<uint8_t>((gg << 2) | (gg >> 4));
        o[2] = static_cast<uint8_t>((b << 2) | (b >> 4)); o[3] = static_cast<uint8_t>((a << 2) | (a >> 4));
        break;
    }
    default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    }
}

const uint8_t kZero[64] = {};

// Resolves an index into an array element pointer; out-of-range reads see zeros.
inline const uint8_t* ArrayElem(int attr, uint32_t idx, uint32_t elemSize, bool& le) {
    const ArrayRef& ar = g.arrays[attr];
    le = ar.le;
    const uint64_t off = static_cast<uint64_t>(idx) * ar.stride;
    if (!ar.data || off + elemSize > ar.size) {
        VGX_LOG_N(16, "vertex array read out of range: attr %d idx %u stride %u size %u\n", attr, idx, ar.stride, ar.size);
        return kZero;
    }
    return ar.data + off;
}

} // namespace

uint32_t StreamVertexSize(int fmt) { return GetPlan(fmt).streamSize; }
const Plan& CurrentPlan(int fmt) { return GetPlan(fmt); }
const Plan& FoldedPlan(int fmt, const Plan& base) { return GetFoldedPlanIn(g_plans, fmt, base); }

// Attribute-major vertex decode: one tight loop per vertex component over the
// whole primitive instead of a per-vertex walk over the components. `src` is
// the big-endian vertex stream (count * p.streamSize bytes), `out` receives
// count * p.layout.stride bytes in the decoded layout. `spans` (optional)
// receives, per indexed attribute, the byte range of its array that was read.
static uint32_t g_lastPosMtxMax = 0;
uint32_t LastDecodedPosMtxMax() { return g_lastPosMtxMax; }

namespace {
// One attribute of `count` vertices in a loop of its own, compiled once for every combination
// of: where an element comes from (IDX 0: the stream itself, 1 / 2: an 8- / 16-bit index into
// an array, with the 32-bit offset test of DecodeVertices), the component type, the number of
// components, and whether a missing second texture coordinate is zeroed. The general loop
// below it keeps all of those in variables; on the console's processor, with its fourteen
// registers, it then reloads eight values from the stack for every vertex and attribute
// (35 instructions where this takes about 20; found in the per-block profile of round 5).
// Same arithmetic, expression for expression, as the general loop: results are bit-identical.
// Returns non-zero when an index pointed outside its array (the element then reads as zeros).
template <int IDX>
inline __attribute__((always_inline)) const uint8_t* AttrElem(const uint8_t* s, const uint8_t* adata, uint32_t astride, uint32_t maxOff, uint32_t& oob) {
    if (IDX == 0) return s;
    const uint32_t idx = IDX == 2 ? be16(s) : s[0];
    const uint32_t off = idx * astride;
    if (off > maxOff) { oob = 1; return kZero; }
    return adata + off;
}
// 8-bit components (the particle quads' corners and texture coordinates): the float of every byte value for one scale (2^-k), looked
// up instead of converted and multiplied: 3 instructions per component where the arithmetic takes 6. The same expression, so the
// same bits. Built when first used; the player's thread is the only one that decodes.
uint32_t g_byteLut[2][32][256];
uint32_t g_byteLutBuilt[2];
inline const uint32_t* ByteLut(bool sign, float scale) {
    const uint32_t k = (127u - (f2u(scale) >> 23)) & 31u; // scale = 1 / (1 << frac), frac < 32
    const int t = sign ? 1 : 0;
    if (!(g_byteLutBuilt[t] & (1u << k))) {
        for (uint32_t b = 0; b < 256; ++b)
            g_byteLut[t][k][b] = f2u((sign ? static_cast<float>(static_cast<int8_t>(static_cast<uint8_t>(b))) : static_cast<float>(b)) * scale);
        g_byteLutBuilt[t] |= 1u << k;
    }
    return g_byteLut[t][k];
}
template <int TYPE, bool LE> inline __attribute__((always_inline)) uint32_t AttrComp(const uint8_t* e, int c, float scale, const uint32_t* lut) {
    // The bit pattern of component c as a float in the decoded layout.
    switch (TYPE) {
    case C_F32: return rd32(e + 4 * c, LE); // (same value as "float * 1.0f" for every input that is not a NaN)
    case C_S16: return f2u(static_cast<float>(static_cast<int16_t>(rd16(e + 2 * c, LE))) * scale);
    case C_U16: return f2u(static_cast<float>(rd16(e + 2 * c, LE)) * scale);
    case C_S8: case C_U8: (void)scale; return lut[e[c]];
    default: return f2u(static_cast<float>(e[c]) * scale);
    }
}
template <int IDX, int TYPE, int N, bool PAD, bool LE, bool POS_DEFAULTS = false>
__attribute__((noinline)) uint32_t DecodeAttr(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata,
                                              uint32_t astride, uint32_t maxOff, float scale) {
    uint32_t oob = 0;
    const uint32_t* const lut = (TYPE == C_S8 || TYPE == C_U8) ? ByteLut(TYPE == C_S8, scale) : nullptr;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        const uint8_t* e = AttrElem<IDX>(s, adata, astride, maxOff, oob);
        const uint32_t w0 = AttrComp<TYPE, LE>(e, 0, scale, lut);
        std::memcpy(o, &w0, 4);
        if (N > 1) { const uint32_t w1 = AttrComp<TYPE, LE>(e, 1, scale, lut); std::memcpy(o + 4, &w1, 4); }
        if (N > 2) { const uint32_t w2 = AttrComp<TYPE, LE>(e, 2, scale, lut); std::memcpy(o + 8, &w2, 4); }
        if (PAD) std::memset(o + 4, 0, 4);
        if (POS_DEFAULTS) {
            if (N == 2) std::memset(o + 8, 0, 4);
            std::memset(o + 12, 0, 4);
        }
    }
    return oob;
}
template <int IDX>
__attribute__((noinline)) uint32_t DecodeRgba8(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata, uint32_t astride,
                                               uint32_t maxOff, float) {
    uint32_t oob = 0;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        const uint8_t* e = AttrElem<IDX>(s, adata, astride, maxOff, oob);
        std::memcpy(o, e, 4);
    }
    return oob;
}
// [IDX][type][components - 1, or 3 for one component with the second zeroed]
#define VGX_ATTR_ROW(I, T, L) {&DecodeAttr<I, T, 1, false, L>, &DecodeAttr<I, T, 2, false, L>, &DecodeAttr<I, T, 3, false, L>, &DecodeAttr<I, T, 1, true, L>}
#define VGX_ATTR_IDX(I, L) {VGX_ATTR_ROW(I, C_U8, L), VGX_ATTR_ROW(I, C_S8, L), VGX_ATTR_ROW(I, C_U16, L), VGX_ATTR_ROW(I, C_S16, L), VGX_ATTR_ROW(I, C_F32, L)}
// [array is little-endian][IDX][type][...]: the stream itself (IDX 0) is always big-endian.
const AttrFn kAttrFn[2][3][5][4] = {{VGX_ATTR_IDX(0, false), VGX_ATTR_IDX(1, false), VGX_ATTR_IDX(2, false)},
                                    {VGX_ATTR_IDX(0, false), VGX_ATTR_IDX(1, true), VGX_ATTR_IDX(2, true)}};
const AttrFn kRgba8Fn[3] = {&DecodeRgba8<0>, &DecodeRgba8<1>, &DecodeRgba8<2>};
#undef VGX_ATTR_ROW
#undef VGX_ATTR_IDX
// The matrix index of the position (PNMTXIDX: stored as the w of the position) and the texture matrix indices (one byte each).
// The highest position matrix index goes to g_lastPosMtxMax.
uint32_t DecodePosMtxIdx(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t*, uint32_t, uint32_t, float) {
    uint32_t mx = 0;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        if (s[0] > mx) mx = s[0];
        const float f = static_cast<float>(s[0]);
        std::memcpy(o, &f, 4); // outOff = 12: the position's w
    }
    g_lastPosMtxMax = mx;
    return 0;
}
uint32_t DecodeTexMtxIdx(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t*, uint32_t, uint32_t, float) {
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) o[0] = s[0];
    return 0;
}
// GXV_DECODE=0: the general loops only; 1 (default): the specialised loops, and for a vertex format whose attributes all come
// straight from the stream the loops chosen when the plan was made (Plan::direct); 2: the latter, each draw also decoded by the
// general loops and compared (logged).
inline int DecodeMode() {
    const int mode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_DECODE", 1))));
    return mode;
}
inline bool SpecialisedDecode() { return DecodeMode() != 0; }

// GXV_POSDEFAULT: 0 (default) retains the old pass; 1 fuses eligible position
// defaults; 2 also compares all 16 position bytes with the old kernel.
// Selected only when making a plan. No environment read or mode branch per vertex/draw.
inline int PositionDefaultsMode() {
    const int mode = static_cast<int>(VGX_KNOB([] {
        const long value = EnvInt("GXV_POSDEFAULT", 0);
        return value == 1 || value == 2 ? static_cast<int>(value) : 0;
    }()));
    return mode;
}
// Like ByteLut and g_lastPosMtxMax, owned by the frontend player thread.
// The finishing thread transforms decoded vertices; it never decodes streams.
uint64_t g_posDefaultDraws = 0, g_posDefaultChecked = 0, g_posDefaultBad = 0;
template <int TYPE, int N>
uint32_t DecodePositionVerify(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os,
                             const uint8_t* adata, uint32_t astride, uint32_t maxOff, float scale) {
    DecodeAttr<0, TYPE, N, false, false, true>(s, o, count, ss, os, adata, astride, maxOff, scale);
    const uint64_t before = g_posDefaultChecked;
    ++g_posDefaultDraws;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        uint8_t ref[16] = {};
        DecodeAttr<0, TYPE, N, false, false>(s, ref, 1, ss, 16, adata, astride, maxOff, scale);
        ++g_posDefaultChecked;
        if (std::memcmp(o, ref, sizeof ref) != 0) {
            ++g_posDefaultBad;
            if (g_posDefaultBad <= 8) std::fprintf(stderr, "[vgx] posdefault: MISMATCH type=%d components=%d\n", TYPE, N);
            std::memcpy(o, ref, sizeof ref);
        }
    }
    if (count && (before == 0 || (g_posDefaultDraws & 0xFFFu) == 0 || (before >> 20) != (g_posDefaultChecked >> 20)))
        std::fprintf(stderr, "[vgx] posdefault: %llu eligible decodes, %llu vertices checked, %llu mismatching\n",
                     static_cast<unsigned long long>(g_posDefaultDraws), static_cast<unsigned long long>(g_posDefaultChecked),
                     static_cast<unsigned long long>(g_posDefaultBad));
    return 0;
}
#define VGX_POS_ROW(T) {&DecodeAttr<0, T, 2, false, false, true>, &DecodeAttr<0, T, 3, false, false, true>}
const AttrFn kPositionFn[5][2] = {VGX_POS_ROW(C_U8), VGX_POS_ROW(C_S8), VGX_POS_ROW(C_U16), VGX_POS_ROW(C_S16), VGX_POS_ROW(C_F32)};
#undef VGX_POS_ROW
#define VGX_POS_ROW(T) {&DecodePositionVerify<T, 2>, &DecodePositionVerify<T, 3>}
const AttrFn kPositionVerifyFn[5][2] = {VGX_POS_ROW(C_U8), VGX_POS_ROW(C_S8), VGX_POS_ROW(C_U16), VGX_POS_ROW(C_S16), VGX_POS_ROW(C_F32)};
#undef VGX_POS_ROW

// NW4R DrawQuad and CharWriter::PrintGlyph write four interleaved vertices. Decode
// them in one traversal instead of a defaults pass and one indirect call per attribute.
template<int N, bool COLOR, bool FIXED>
bool DecodeDirectQuad(const Plan& p, const uint8_t* src, uint8_t* out) {
    constexpr int tex = 4*N + (COLOR ? 4 : 0);
    constexpr int stride = tex + (FIXED ? 4 : 8);
    const int dstStride = p.layout.stride, texOut = p.layout.offTex[0];
    const float scale = FIXED ? p.a[COLOR ? 2 : 1].scale : 1.f;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* s = src + i*stride;
        uint8_t* d = out + i*dstStride;
        const uint32_t pos[4] = {be32(s), be32(s+4), N == 3 ? be32(s+8) : 0u, 0u};
        std::memcpy(d, pos, sizeof pos);
        if (COLOR) std::memcpy(d + p.layout.offCol0, s + 4*N, 4);
        if (FIXED) {
            const float st[2] = {static_cast<float>(be16(s+tex))*scale, static_cast<float>(be16(s+tex+2))*scale};
            std::memcpy(d + texOut, st, sizeof st);
        } else {
            const uint32_t st[2] = {be32(s+tex), be32(s+tex+4)};
            std::memcpy(d + texOut, st, sizeof st);
        }
    }
    return true;
}
// JPABaseShape::jpa_dl addresses four consecutive corners of two byte arrays.
// A changed index, stride or undersized binding goes back through the ordinary
// bounds and reporting path before a single output byte is written.
static bool DecodeParticleQuad(const Plan& p, const uint8_t* src, uint8_t* out) {
    static constexpr uint8_t corners[] = {0,0,1,1,2,2,3,3};
    const auto& pos = g.arrays[A_POS];
    const auto& tex = g.arrays[A_TEX0];
    if (std::memcmp(src, corners, sizeof corners) || pos.stride != 3 || tex.stride != 2 ||
        pos.size < 12 || tex.size < 8 || !pos.data || !tex.data) return false;
    for (int i=0; i<4; ++i) {
        const auto* v = reinterpret_cast<const int8_t*>(pos.data + 3*i);
        const auto* t = reinterpret_cast<const int8_t*>(tex.data + 2*i);
        const float xyzw[] = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), 0.f};
        const float st[] = {static_cast<float>(t[0]), static_cast<float>(t[1])};
        uint8_t* d = out + i*p.layout.stride;
        std::memcpy(d, xyzw, sizeof xyzw);
        std::memcpy(d + p.layout.offTex[0], st, sizeof st);
    }
    return true;
}
decltype(Plan::quad) QuadRecipe(const Plan& p) {
    if (p.n == 2 && p.a[0].attr == A_POS && p.a[1].attr == A_TEX0 && p.a[0].mode == T_INDEX8 && p.a[1].mode == T_INDEX8 &&
        p.a[0].type == C_S8 && p.a[1].type == C_S8 && p.a[0].comps == 3 && p.a[1].comps == 2 &&
        p.a[0].frac == 0 && p.a[1].frac == 0) return DecodeParticleQuad;
    if (!p.direct || (p.n != 2 && p.n != 3)) return nullptr;
    const auto& pos = p.a[0];
    const auto& tex = p.a[p.n-1];
    if (pos.attr != A_POS || pos.type != C_F32 || tex.attr != A_TEX0 || tex.comps != 2 ||
        (tex.type != C_F32 && tex.type != C_U16)) return nullptr;
    const bool color = p.n == 3;
    if (color && (p.a[1].attr != A_CLR0 || p.a[1].type != 5)) return nullptr;
    static constexpr decltype(Plan::quad) recipes[2][2][2] = {
        {{DecodeDirectQuad<2,false,false>, DecodeDirectQuad<2,false,true>}, {DecodeDirectQuad<2,true,false>, DecodeDirectQuad<2,true,true>}},
        {{DecodeDirectQuad<3,false,false>, DecodeDirectQuad<3,false,true>}, {DecodeDirectQuad<3,true,false>, DecodeDirectQuad<3,true,true>}}
    };
    return recipes[pos.comps == 3][color][tex.type == C_U16];
}
} // namespace

// Picks, for a plan whose attributes all come from the stream, the decode loop of each (the choice the general path makes per draw).
static void FinishDirectPlan(Plan& p) {
    bool direct = true, needZ = true, needW = true;
    for (int i = 0; i < p.n; ++i) {
        AttrPlan& a = p.a[i];
        a.fn = a.fnx[0] = a.fnx[1] = nullptr;
        a.scale = 1.0f;
        if (a.attr == A_POS && a.comps == 3) needZ = false;
        if (a.attr == 0) needW = false;
        if (a.attr == 0) a.fn = &DecodePosMtxIdx;
        else if (a.attr <= 8) a.fn = &DecodeTexMtxIdx;
        else {
            // (the choice the general path makes per draw: the loop for this source, type and component count)
            const int im = a.mode == T_DIRECT ? 0 : (a.mode == T_INDEX16 ? 2 : 1);
            if (a.attr == A_CLR0 || a.attr == A_CLR1) {
                if (a.type == 5) a.fn = a.fnx[0] = a.fnx[1] = kRgba8Fn[im];
            } else {
                const int n = a.attr == A_NRM ? 3 : a.comps;
                const bool padT = a.attr >= A_TEX0 && n == 1;
                a.scale = 1.0f / static_cast<float>(1u << a.frac);
                const int type = a.type <= C_F32 ? a.type : C_U8;
                const int form = padT ? 3 : (n >= 1 && n <= 3 ? n - 1 : 2);
                a.fn = kAttrFn[0][im][type][form];
                a.fnx[0] = kAttrFn[0][im][type][form];
                a.fnx[1] = kAttrFn[1][im][type][form];
            }
            if (a.mode != T_DIRECT) a.fn = nullptr; // (an indexed attribute needs its array: fnx)
        }
        if (!a.fn) direct = false;
    }
    p.direct = direct;
    p.defaults = static_cast<uint8_t>((needZ ? 1u : 0u) | (needW ? 2u : 0u));
    p.directDefaults = p.defaults;
    // Without a matrix-index attribute, direct POS2/POS3 can supply all defaults
    // in their existing traversal. TMI still needs its own clearing pass.
    const int positionMode = PositionDefaultsMode();
    if (positionMode != 2 && VGX_KNOB(EnvInt("GXV_QUAD_DECODE", 1) != 0))
        p.quad = QuadRecipe(p);
    if (positionMode && direct && needW && !(p.layoutMask & VL_TMI)) {
        for (int i = 0; i < p.n; ++i) {
            AttrPlan& a = p.a[i];
            if (a.attr == A_POS && a.type <= C_F32 && (a.comps == 2 || a.comps == 3)) {
                a.fn = positionMode == 2 ? kPositionVerifyFn[a.type][a.comps - 2] : kPositionFn[a.type][a.comps - 2];
                p.directDefaults = 0;
                break;
            }
        }
    }
}

// Inspect indices without reading the array bytes, so caches can snapshot them before decoding.
ArraySpans MeasureArraySpans(const Plan& p, const uint8_t* src, uint32_t count, bool* complete) {
    ArraySpans spans;
    if (complete) *complete = true;
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        if (a.attr < A_POS || a.mode == T_DIRECT) continue;
        const ArrayRef& ar = g.arrays[a.attr];
        const uint32_t elem = (a.attr == A_NRM && a.cnt == 2) ? 3u * static_cast<uint32_t>(CompSize(a.type)) : a.elemSize;
        const uint64_t size = ar.data ? ar.size : 0;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        const uint8_t* s = src + a.srcOff;
        for (uint32_t v = 0; v < count; ++v, s += p.streamSize) {
            const uint32_t idx = a.mode == T_INDEX16 ? be16(s) : s[0];
            const uint64_t off = static_cast<uint64_t>(idx) * ar.stride;
            if (off + elem > size) {
                if (complete) *complete = false;
                continue;
            }
            if (idx < lo) lo = idx;
            if (idx > hi) hi = idx;
        }
        if (lo <= hi) {
            spans.mask |= 1u << a.attr;
            spans.lo[a.attr - A_POS] = lo * ar.stride;
            spans.hi[a.attr - A_POS] = hi * ar.stride + elem;
        }
    }
    return spans;
}

// The general form: every attribute from the stream or from an array, chosen per draw. `spec`: the specialised loops (kAttrFn) where they apply.
// `arrays`, `player` (GXV_FE_PARALLEL, DecodeVerticesOff): the vertex arrays to read; false: not on the player's thread (no global is
// written: the highest matrix row is left alone, an out-of-range index is counted in *oobOut instead of logged).
static void DecodeVerticesGeneral(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans, bool spec,
                                  const ArrayRef* arrays = g.arrays, bool player = true, uint32_t* oobOut = nullptr) {
    const VtxLayout& L = p.layout;
    if (player) g_lastPosMtxMax = 0;
    const uint32_t ss = p.streamSize, os = L.stride;
    if (spans) *spans = MeasureArraySpans(p, src, count);
    // Defaults: z = 0 for two-component positions, matrix row 0, texture-matrix indices 0.
    // (Skipped for what an attribute of this plan writes for every vertex anyway: z by a
    // three-component position, the matrix row by PNMTXIDX.)
    {
        const bool needZ = !spec || (p.defaults & 1u) != 0, needW = !spec || (p.defaults & 2u) != 0;
        const bool tmi = (L.mask & VL_TMI) != 0;
        if (needZ || needW || tmi) {
            uint8_t* o = out;
            for (uint32_t v = 0; v < count; ++v, o += os) {
                if (needZ) std::memset(o + 8, 0, 4);
                if (needW) std::memset(o + 12, 0, 4);
                if (tmi) std::memset(o + L.offTmi, 0, 8);
            }
        }
    }
    uint32_t srcOff = 0;
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        const uint8_t* s = src + srcOff;
        srcOff += a.srcSize;
        uint8_t* o = out + a.outOff;
        if (a.attr <= 8) {
            if (a.attr == 0) {
                uint32_t mx = 0;
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    if (s[0] > mx) mx = s[0];
                    const float f = static_cast<float>(s[0]);
                    std::memcpy(o, &f, 4); // outOff = 12: the position's w
                }
                if (player) g_lastPosMtxMax = mx;
            } else {
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) o[0] = s[0];
            }
            continue;
        }
        // Source element for vertex v: the stream itself (direct) or an array element (indexed).
        const ArrayRef& ar = arrays[a.attr];
        const bool indexed = a.mode != T_DIRECT;
        const bool idx16 = a.mode == T_INDEX16;
        const uint32_t elem = (a.attr == A_NRM && a.cnt == 2) ? 3u * static_cast<uint32_t>(CompSize(a.type)) : a.elemSize; // NBT3: normal only
        const bool le = indexed && ar.le; // (the stream is big-endian; an array is what its owner says)
        const uint8_t* const adata = ar.data;
        const uint32_t astride = ar.stride;
        const uint32_t size32 = adata ? ar.size : 0;
        const uint64_t asize = size32;
        bool oob = false;
        // An index is at most 65535, so with a stride below 65536 its offset fits 32 bits and
        // one unsigned comparison says whether the element lies inside the array (the 64-bit
        // form was 44 instructions per vertex and attribute with the range bookkeeping below).
        const bool narrow = astride <= 0xFFFFu && size32 >= elem; // (the size is 32 bits)
        const uint32_t maxOff = narrow ? size32 - elem : 0;
        if ((!indexed || narrow) && spec) {
            const AttrFn f = indexed ? a.fnx[le ? 1 : 0] : a.fn; // (null: a colour that is not RGBA8)
            if (f) {
                const uint32_t bad = f(s, o, count, ss, os, adata, astride, maxOff, a.scale);
                if (bad && !player) ++*oobOut;
                else if (bad) VGX_LOG_N(16, "vertex array read out of range: attr %d stride %u size %u\n", a.attr, ar.stride, ar.size);
                continue;
            }
        }
#define VGX_ELEM(e)                                                                                   \
        const uint8_t* e;                                                                             \
        if (indexed) {                                                                                \
            const uint32_t idx = idx16 ? be16(s) : s[0];                                              \
            if (narrow) {                                                                             \
                const uint32_t off = idx * astride;                                                   \
                if (off > maxOff) { e = kZero; oob = true; }                                          \
                else e = adata + off;                                                                 \
            } else {                                                                                  \
                const uint64_t off = static_cast<uint64_t>(idx) * astride;                            \
                if (off + elem > asize) { e = kZero; oob = true; }                                    \
                else e = adata + off;                                                                 \
            }                                                                                         \
        } else e = s;
// One store per component (a memcpy of n * 4 bytes with n not a constant is a call into
// the C library for every vertex: 12 500 calls per open-play frame).
#define VGX_STORE(src)                                    \
        std::memcpy(o, &src[0], 4);                       \
        if (n > 1) std::memcpy(o + 4, &src[1], 4);        \
        if (n > 2) std::memcpy(o + 8, &src[2], 4);
        if (a.attr == A_CLR0 || a.attr == A_CLR1) {
            if (a.type == 5) { // RGBA8
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) { VGX_ELEM(e) std::memcpy(o, e, 4); }
            } else {
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) { VGX_ELEM(e) DecodeColor(e, a.type, o, le && e != kZero); }
            }
        } else {
            // POS: 2 or 3 components; NRM: 3 (of 3 or 9); TEXn: 1 or 2 (the second defaults to 0).
            const int n = a.attr == A_NRM ? 3 : a.comps;
            const bool padT = a.attr >= A_TEX0 && n == 1;
            const float scale = 1.0f / static_cast<float>(1u << a.frac);
            switch (a.type) {
            case C_F32:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    uint32_t w[3];
                    w[0] = rd32(e, le);
                    if (n > 1) w[1] = rd32(e + 4, le);
                    if (n > 2) w[2] = rd32(e + 8, le);
                    // Same value as the generic path's "float * 1.0f" for every non-NaN input.
                    VGX_STORE(w)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_S16:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(static_cast<int16_t>(rd16(e, le))) * scale;
                    if (n > 1) f[1] = static_cast<float>(static_cast<int16_t>(rd16(e + 2, le))) * scale;
                    if (n > 2) f[2] = static_cast<float>(static_cast<int16_t>(rd16(e + 4, le))) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_U16:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(rd16(e, le)) * scale;
                    if (n > 1) f[1] = static_cast<float>(rd16(e + 2, le)) * scale;
                    if (n > 2) f[2] = static_cast<float>(rd16(e + 4, le)) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_S8:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(static_cast<int8_t>(e[0])) * scale;
                    if (n > 1) f[1] = static_cast<float>(static_cast<int8_t>(e[1])) * scale;
                    if (n > 2) f[2] = static_cast<float>(static_cast<int8_t>(e[2])) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            default: // C_U8
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(e[0]) * scale;
                    if (n > 1) f[1] = static_cast<float>(e[1]) * scale;
                    if (n > 2) f[2] = static_cast<float>(e[2]) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            }
        }
#undef VGX_ELEM
#undef VGX_STORE
        if (oob && !player) ++*oobOut;
        else if (oob) VGX_LOG_N(16, "vertex array read out of range: attr %d stride %u size %u\n", a.attr, ar.stride, ar.size);
    }
}


// The fast form, for a plan whose attributes all come from the stream (Plan::direct): no array, no index, no per-draw choices.
static void DecodeVerticesDirect(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, bool player = true) {
    const VtxLayout& L = p.layout;
    if (player) g_lastPosMtxMax = 0; // (GXV_FE_PARALLEL: off the player's thread, a plan without PNMTXIDX: nothing else is written)
    if (count == 4 && p.quad) { p.quad(p, src, out); return; }
    const uint32_t ss = p.streamSize, os = L.stride;
    // Defaults: z = 0 for two-component positions, matrix row 0 (no PNMTXIDX), texture-matrix indices 0.
    const bool needZ = (p.directDefaults & 1u) != 0, needW = (p.directDefaults & 2u) != 0;
    if ((L.mask & VL_TMI) != 0) {
        uint8_t* o = out;
        for (uint32_t v = 0; v < count; ++v, o += os) {
            if (needZ) std::memset(o + 8, 0, 4);
            if (needW) std::memset(o + 12, 0, 4);
            std::memset(o + L.offTmi, 0, 8);
        }
    } else if (needZ && needW) {
        uint8_t* o = out + 8;
        for (uint32_t v = 0; v < count; ++v, o += os) { std::memset(o, 0, 4); std::memset(o + 4, 0, 4); }
    } else if (needW) {
        uint8_t* o = out + 12;
        for (uint32_t v = 0; v < count; ++v, o += os) std::memset(o, 0, 4);
    } else if (needZ) {
        uint8_t* o = out + 8;
        for (uint32_t v = 0; v < count; ++v, o += os) std::memset(o, 0, 4);
    }
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        if (!player && a.attr == 0) { // (GXV_FE_PARALLEL: DecodePosMtxIdx's loop without its write of the highest row)
            const uint8_t* s = src + a.srcOff;
            uint8_t* o = out + a.outOff;
            for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                const float f = static_cast<float>(s[0]);
                std::memcpy(o, &f, 4);
            }
            continue;
        }
        a.fn(src + a.srcOff, out + a.outOff, count, ss, os, nullptr, 0, 0, a.scale);
    }
}

// The decode the game's vertex streams get: the direct loops or the specialised general loops.
static void DecodeVerticesFast(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans) {
    if (p.direct) {
        if (spans) *spans = ArraySpans{};
        DecodeVerticesDirect(p, src, count, out);
    } else if (count == 4 && !spans && p.quad && p.quad(p, src, out)) {
        g_lastPosMtxMax = 0;
    } else {
        DecodeVerticesGeneral(p, src, count, out, spans, true);
    }
}

void DecodeVertices(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans) {
    const int mode = DecodeMode();
    if (mode == 0) { DecodeVerticesGeneral(p, src, count, out, spans, false); return; }
    if (mode == 1 || !diag::kEnabled) { DecodeVerticesFast(p, src, count, out, spans); return; }
    // GXV_DECODE=2: both, and compare (the vertices, and the highest matrix index) against the general loops with nothing specialised.
    // The destination is filled with a pattern first so that a byte neither form writes does not count; 16 guard bytes follow the vertices.
    static std::vector<uint8_t> fast, ref;
    static uint64_t draws = 0, bad = 0, directDraws = 0;
    const size_t bytes = static_cast<size_t>(count) * p.layout.stride;
    fast.assign(bytes + 16, 0xA5);
    ref.assign(bytes + 16, 0xA5);
    ArraySpans spansFast{}, spansRef{};
    if (p.direct) ++directDraws;
    DecodeVerticesFast(p, src, count, fast.data(), spans ? &spansFast : nullptr);
    const uint32_t mxFast = g_lastPosMtxMax;
    DecodeVerticesGeneral(p, src, count, ref.data(), spans ? &spansRef : nullptr, false);
    const uint32_t mxRef = g_lastPosMtxMax;
    bool ok = std::memcmp(fast.data(), ref.data(), bytes + 16) == 0 && mxFast == mxRef;
    if (ok && spans && !p.direct) ok = spansFast.mask == spansRef.mask && std::memcmp(spansFast.lo, spansRef.lo, sizeof spansFast.lo) == 0 && std::memcmp(spansFast.hi, spansRef.hi, sizeof spansFast.hi) == 0;
    ++draws;
    if (!ok) {
        ++bad;
        VGX_LOG_N(16, "decode: MISMATCH (mask %03X, %u vertices, stride %u, direct %d)\n", p.layoutMask, count, static_cast<unsigned>(p.layout.stride), p.direct ? 1 : 0);
    }
    if ((draws & 0x3FFFu) == 0)
        VGX_LOG_N(200, "decode: %llu draws checked (%llu direct), %llu mismatching\n", static_cast<unsigned long long>(draws),
                  static_cast<unsigned long long>(directDraws), static_cast<unsigned long long>(bad));
    std::memcpy(out, fast.data(), bytes);
    if (spans && !p.direct) *spans = spansFast;
    else if (spans) *spans = ArraySpans{};
    g_lastPosMtxMax = mxFast;
}

// ---- GXV_FE_PARALLEL (vgx_fepar.h): the decode on the finishing thread
bool DecodeOffThreadOk(const Plan& p) {
    // (GXV_DECODE=0/2, GXV_POSDEFAULT=2: loops that write globals. PNMTXIDX is fine: the decode off the player's thread does not write the
    // highest row, the player reads it from the stream itself, PosMtxMaxOf)
    return p.valid && DecodeMode() == 1 && PositionDefaultsMode() != 2;
}
uint32_t PosMtxMaxOf(const Plan& p, const uint8_t* src, uint32_t count) {
    // (the highest PNMTXIDX of the draw, as the decode works it out: DecodePosMtxIdx / DecodeVerticesGeneral; 0 without one)
    uint32_t off = 0;
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        if (a.attr == 0) {
            uint32_t mx = 0;
            const uint8_t* s = src + off;
            for (uint32_t v = 0; v < count; ++v, s += p.streamSize)
                if (s[0] > mx) mx = s[0];
            return mx;
        }
        off += a.srcSize;
    }
    return 0;
}
void DecodeVerticesOff(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, const ArrayRef* arrays, uint32_t* oob) {
    // (DecodeVerticesFast with the player's flag off: the same loops, the same arithmetic)
    if (p.direct) DecodeVerticesDirect(p, src, count, out, false);
    else DecodeVerticesGeneral(p, src, count, out, nullptr, true, arrays, false, oob);
}
void DecodeTablesReady() {
    // (ByteLut fills a table on its first use: every scale 2^-k the plans can ask for, both signs, here and now on the player)
    for (uint32_t k = 0; k < 32; ++k) {
        const float scale = 1.0f / static_cast<float>(1u << k);
        (void)ByteLut(false, scale);
        (void)ByteLut(true, scale);
    }
}

void SyncVertexArraysSlow() { SyncArrays(0x0FFF); }
// GXV_FOLDAHEAD=2: checks every vertex decoded into the folded layout against the plain decode laid out again
// the way register folding used to (all but the two colour slots, which EmitDraw fills either way); logged.
static uint64_t g_foldRedo = 0; // guessed folds that did not fold
static void FoldAheadCheck(const Plan& p, const Plan& fp, const uint8_t* src, uint32_t count, const uint8_t* folded) {
    static std::vector<uint8_t> plain;
    static uint64_t draws = 0, bad = 0;
    plain.resize(static_cast<size_t>(count) * p.layout.stride);
    const uint32_t keep = g_lastPosMtxMax;
    DecodeVertices(p, src, count, plain.data(), nullptr);
    g_lastPosMtxMax = keep;
    const uint32_t oldWords = p.layout.stride / 4u, newWords = fp.layout.stride / 4u;
    const uint32_t head = 4u + ((p.layoutMask & VL_NRM) ? 3u : 0u);
    const uint32_t oldCols = ((p.layoutMask & VL_COL0) ? 1u : 0u) + ((p.layoutMask & VL_COL1) ? 1u : 0u);
    const uint32_t rest = oldWords - head - oldCols;
    bool ok = newWords == head + 2u + rest;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const uint8_t* in = plain.data() + static_cast<size_t>(i) * oldWords * 4u;
        const uint8_t* out = folded + static_cast<size_t>(i) * newWords * 4u;
        ok = std::memcmp(in, out, head * 4u) == 0 && std::memcmp(in + (head + oldCols) * 4u, out + (head + 2u) * 4u, rest * 4u) == 0;
    }
    ++draws;
    if (!ok) {
        ++bad;
        VGX_LOG_N(8, "foldahead: MISMATCH (mask %03X, %u vertices, stride %u -> %u)\n", p.layoutMask, count, static_cast<unsigned>(p.layout.stride),
                  static_cast<unsigned>(fp.layout.stride));
    }
    if ((draws & 0x3FFFu) == 0)
        VGX_LOG_N(200, "foldahead: %llu draws checked, %llu mismatching, %llu guesses missed\n", static_cast<unsigned long long>(draws),
                  static_cast<unsigned long long>(bad), static_cast<unsigned long long>(g_foldRedo));
}

// GXV_FXMERGE (FxMergeRun below): within one of the game's particle commands every call of the same one-quad list decodes the same
// bytes of the same arrays with the same plan (the command holds nothing else but TEV colour writes and matrix loads, which a decode
// does not read; anything else, a list that goes through CallList for one, ends the reuse). So the vertices the first such draw
// decoded are copied for the others instead of decoded again. Only the bytes a decode writes are copied: they are found once per plan
// by decoding into two buffers filled differently (a byte the decode writes is the same in both); the others (the colour slots of a
// folded layout, which EmitDraw fills) are left as the arena has them, as a decode leaves them. The decode's own result (its highest
// matrix row) goes with the copy.
namespace {
struct FxMemo {
    bool on = false;    // (set around FxTinyDraw's DrawStreamPlan when the command has enough calls)
    bool valid = false; // (the entry below is the last decode of this command)
    const Plan* plan = nullptr;
    const uint8_t* src = nullptr;
    uint32_t count = 0, srcLen = 0, bytes = 0, posMtxMax = 0;
    int runs = -1;                 // the run list's entry in g_fxRuns, -1 none
    uint32_t runsGen = 0;          // (that entry's generation: it may have been filled again since)
    uint8_t srcBytes[64];          // the vertex bytes of the list as decoded
    alignas(16) uint8_t verts[16 * 112];
} g_fxMemo;
// Which bytes of a decode of `count` vertices with a plan are written: runs of (offset, length), per plan (compared whole) and count.
struct FxRuns {
    Plan plan;
    uint32_t count = 0, gen = 0;
    uint16_t n = 0;
    uint16_t run[64][2];
};
FxRuns g_fxRuns[4];
uint32_t g_fxRunsNext = 0, g_fxRunsUsed = 0, g_fxRunsGen = 0;
uint64_t g_fxMemoHits = 0;
// (-1: more runs than an entry holds, or too many bytes: no reuse for this plan)
int FxRunsOf(const Plan& p, const uint8_t* src, uint32_t count, uint32_t bytes) {
    for (uint32_t i = 0; i < g_fxRunsUsed; ++i)
        if (g_fxRuns[i].count == count && std::memcmp(&g_fxRuns[i].plan, &p, sizeof(Plan)) == 0) return g_fxRuns[i].n ? static_cast<int>(i) : -1;
    static uint8_t a[16 * 112], b[16 * 112];
    std::memset(a, 0x00, bytes);
    std::memset(b, 0xFF, bytes);
    const uint32_t keep = g_lastPosMtxMax;
    DecodeVertices(p, src, count, a, nullptr);
    DecodeVertices(p, src, count, b, nullptr);
    g_lastPosMtxMax = keep;
    const uint32_t i = g_fxRunsNext;
    g_fxRunsNext = (g_fxRunsNext + 1) & 3u;
    if (g_fxRunsUsed < 4) ++g_fxRunsUsed;
    FxRuns& r = g_fxRuns[i];
    std::memcpy(&r.plan, &p, sizeof(Plan));
    r.count = count;
    r.gen = ++g_fxRunsGen;
    r.n = 0;
    for (uint32_t o = 0; o < bytes;) {
        if (a[o] != b[o]) { ++o; continue; }
        uint32_t e = o;
        while (e < bytes && a[e] == b[e]) ++e;
        if (r.n == 64) { r.n = 0; return -1; }
        r.run[r.n][0] = static_cast<uint16_t>(o);
        r.run[r.n][1] = static_cast<uint16_t>(e - o);
        ++r.n;
        o = e;
    }
    return r.n ? static_cast<int>(i) : -1;
}
} // namespace
// DecodeVertices for DrawStreamPlan (no spans), through the memo while a particle command's short draw is decoding.
static inline void DecodeForDraw(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out) {
    if (__builtin_expect(!g_fxMemo.on, 1)) { DecodeVertices(p, src, count, out, nullptr); return; }
    FxMemo& m = g_fxMemo;
    const uint32_t bytes = count * p.layout.stride, sl = count * p.streamSize;
    if (m.valid && m.plan == &p && m.src == src && m.count == count && m.bytes == bytes && m.srcLen == sl && std::memcmp(m.srcBytes, src, sl) == 0 &&
        g_fxRuns[m.runs].gen == m.runsGen) {
        const FxRuns& r = g_fxRuns[m.runs];
        for (uint32_t i = 0; i < r.n; ++i) std::memcpy(out + r.run[i][0], m.verts + r.run[i][0], r.run[i][1]);
        g_lastPosMtxMax = m.posMtxMax;
        ++g_fxMemoHits;
        return;
    }
    m.valid = false;
    DecodeVertices(p, src, count, out, nullptr);
    if (bytes > sizeof m.verts || sl > sizeof m.srcBytes || sl == 0) return;
    const uint32_t mx = g_lastPosMtxMax;
    const int runs = FxRunsOf(p, src, count, bytes);
    g_lastPosMtxMax = mx;
    if (runs < 0) return;
    m.plan = &p;
    m.src = src;
    m.count = count;
    m.bytes = bytes;
    m.srcLen = sl;
    m.posMtxMax = mx;
    m.runs = runs;
    m.runsGen = g_fxRuns[runs].gen;
    std::memcpy(m.srcBytes, src, sl);
    std::memcpy(m.verts, out, bytes);
    m.valid = true;
}

// (GXV_FE_SPLIT / GXV_WORKLOAD_PROBE: what ProcessStream parses, for the "fe stream detail" line of vgx_workload.h. Plain counters of the player's
// thread, added to once per stream or per draw command; read from another thread for the log, so a figure may be a call behind.)
namespace {
struct StreamDetail {
    uint64_t calls[2] = {}, bytes[2] = {}, stateCmds[2] = {}, draws[2] = {}, verts[2] = {};
    uint64_t flushStrips = 0, bypassLists = 0, bypassBytes = 0;
} g_sd;
} // namespace
void NoteBypassStream(uint32_t nbytes) { ++g_sd.bypassLists; g_sd.bypassBytes += nbytes; }
void StreamDetailCounters(uint64_t out[13]) {
    for (int i = 0; i < 2; ++i) {
        out[i * 5 + 0] = g_sd.calls[i]; out[i * 5 + 1] = g_sd.bytes[i]; out[i * 5 + 2] = g_sd.stateCmds[i];
        out[i * 5 + 3] = g_sd.draws[i]; out[i * 5 + 4] = g_sd.verts[i];
    }
    out[10] = g_sd.flushStrips; out[11] = g_sd.bypassLists; out[12] = g_sd.bypassBytes;
}

// ---- streamc31: GXV_DYN_CENSUS bit 2, the stream-decode census (vgx_deccen.h; census only, changes nothing drawn)
namespace {
#if defined(VGX_CLONE_B)
constexpr bool DecCenOn() { return false; } // (GXV_FE_TWOCORE's second worker draws nothing)
#else
int g_decCen = -1;
inline bool DecCenOn() {
    if (__builtin_expect(g_decCen < 0, 0)) g_decCen = (EnvInt("GXV_DYN_CENSUS", 0) & 2) != 0 ? 1 : 0;
    return g_decCen != 0;
}
deccen::Census* DecCen() {
    static deccen::Census* c = new deccen::Census(&DecCenNs, static_cast<size_t>(std::clamp<long>(EnvInt("GXV_DYN_CENSUS_KB", 1024), 16, 16384)) << 10);
    return c;
}
#endif
// What DrawStreamPlan did for one draw (the census's part; plain fields, nothing is read unless the census is on).
struct DcDraw {
    uint64_t decNs = 0, emitNs = 0;
    uint32_t decN = 0, emitN = 0, mask = 0;
    const uint8_t* out = nullptr;
    uint32_t outBytes = 0;
};
// The plan signature: everything of the plan the decode reads (per attribute where it comes from, its type, size and place), the primitive,
// the vertex count and the layout decoded into. Not the plan's bytes: its padding is not initialised.
uint64_t DecCenSig(const Plan& p, uint8_t prim, uint32_t count, uint32_t mask) {
    uint64_t w[2 + 21 * 2];
    int n = 0;
    w[n++] = (static_cast<uint64_t>(prim) << 56) | (static_cast<uint64_t>(p.direct) << 48) | (static_cast<uint64_t>(p.streamSize) << 32) | count;
    w[n++] = (static_cast<uint64_t>(p.layoutMask) << 32) | mask;
    for (int i = 0; i < p.n && i < 21; ++i) {
        const AttrPlan& a = p.a[i];
        w[n++] = (static_cast<uint64_t>(a.attr) << 56) | (static_cast<uint64_t>(a.mode) << 48) | (static_cast<uint64_t>(a.type) << 40) |
                 (static_cast<uint64_t>(a.cnt) << 32) | (static_cast<uint64_t>(a.frac) << 24) | (static_cast<uint64_t>(a.srcSize) << 16) | a.srcOff;
        uint32_t sb;
        std::memcpy(&sb, &a.scale, 4);
        w[n++] = (static_cast<uint64_t>(a.elemSize) << 48) | (static_cast<uint64_t>(a.outOff) << 40) | (static_cast<uint64_t>(a.comps) << 32) | sb;
    }
    return XXH3_64bits(w, static_cast<size_t>(n) * sizeof w[0]);
}
// (a copy of the decoded vertices, for the census's timed copy-out; the arena may move during EmitDraw)
void DecCenHold(DcDraw& r, const uint8_t* out, uint32_t bytes) {
    static std::vector<uint8_t> hold;
    hold.assign(out, out + bytes);
    r.out = hold.data();
    r.outBytes = bytes;
}
void DecCenNote(const Plan& p, uint8_t prim, const uint8_t* src, uint32_t count, uint8_t origin, const DcDraw& r) {
#if !defined(VGX_CLONE_B)
    deccen::DrawIn d;
    d.sig = DecCenSig(p, prim, count, r.mask);
    d.origin = origin;
    d.allDirect = p.direct;
    d.count = count;
    d.streamSize = p.streamSize;
    d.stream = src;
    d.out = r.out;
    d.outBytes = r.outBytes;
    d.decodeNs = r.decNs; d.decodeScopes = r.decN;
    d.emitNs = r.emitNs; d.emitScopes = r.emitN;
    bool complete = true;
    const ArraySpans sp = p.direct ? ArraySpans{} : MeasureArraySpans(p, src, count, &complete);
    d.inRange = complete;
    for (int i = 0; i < p.n && i < 21; ++i) {
        const AttrPlan& a = p.a[i];
        deccen::AttrIn& o = d.a[d.nAttr++];
        o.attr = a.attr;
        o.srcOff = a.srcOff;
        o.srcSize = a.srcSize;
        o.indexed = a.attr >= A_POS && a.mode != T_DIRECT;
        if (o.indexed) {
            const ArrayRef& ar = g.arrays[a.attr];
            o.stride = ar.stride;
            o.le = ar.le;
            if (ar.data && (sp.mask >> a.attr & 1u)) {
                o.arr = ar.data + sp.lo[a.attr - A_POS];
                o.arrLen = sp.hi[a.attr - A_POS] - sp.lo[a.attr - A_POS];
            }
        }
    }
    DecCen()->Note(d);
#else
    (void)p; (void)prim; (void)src; (void)count; (void)origin; (void)r;
#endif
}
} // namespace
void DecCenPicture() {
#if !defined(VGX_CLONE_B)
    if (DecCenOn()) DecCen()->EndPicture();
#endif
}
void DecCenLog(void (*emit)(const char* line)) {
#if !defined(VGX_CLONE_B)
    if (!DecCenOn()) return;
    char lines[6][1000];
    const int n = DecCen()->Lines(lines, 6);
    for (int i = 0; i < n; ++i) emit(lines[i]);
#else
    (void)emit;
#endif
}

// (the caller has the plan of `fmt` already: the command parser asked for the vertex size)
// `origin` (GXV_DYN_CENSUS bit 2 only): 0 the top-level stream, 1 a list ProcessStream parses, 2 a particle command's list (FxTinyDraw).
static void DrawStreamPlan(uint8_t prim, int fmt, const Plan& p, const uint8_t* src, uint32_t count, uint32_t avail, uint8_t origin) {
#if defined(VGX_CLONE_B)
    // (GXV_FE_TWOCORE, the second worker: its scan leaves draws out (they change no register); a segment never reaches one)
    if (two::g_prescan) return;
    two::Abandon(1);
#endif
    workload::FeScope feDraw(workload::FeDraw);
    SyncVertexArrays();
    if (count == 0 || p.streamSize == 0) return;
    // The console's GX library sends a triangle strip of all-zero bytes in front of some display
    // lists (__GXSendFlushPrim: a workaround for the transform unit, asked for with
    // GXSetMisc(GX_MT_XF_FLUSH) by the particle and shadow code: half of a title frame's
    // primitives). Every vertex of it is the same one (indices and coordinates all zero), so it
    // covers no pixel; it is not worth a state resolution and a draw record.
    if (prim == 0x98 && count <= 16) {
        const uint32_t n = count * p.streamSize;
        if (n <= avail) {
            uint32_t i = 0;
            for (; i + 4 <= n; i += 4) {
                uint32_t w;
                std::memcpy(&w, src + i, 4);
                if (w) break;
            }
            while (i < n && src[i] == 0) ++i;
            if (i == n) { ++g_sd.flushStrips; return; }
        }
    }
    if (static_cast<uint64_t>(count) * p.streamSize > avail) {
        VGX_LOG_N(16, "draw truncated: %u verts x %u bytes > %u available\n", count, p.streamSize, avail);
        count = avail / p.streamSize;
        if (count == 0) return;
    }
    // (GXV_DYN_CENSUS bit 2: the decodes' and EmitDraws' times and the vertices made, for DecCenNote; nothing else changes)
    const bool dc = DecCenOn();
    DcDraw dcr;
    // (after a run of draws that folded their registers: decode into the folded layout at once)
    const uint32_t decodeMask = FoldGuess(p.layoutMask, count);
    if (decodeMask != p.layoutMask) {
        const Plan& fp = FoldedPlan(fmt, p);
        uint8_t* fout = BeginVertices(decodeMask, count);
        if (!fout) return;
        const uint64_t t0 = dc ? DecCenNs() : 0;
        DecodeForDraw(fp, src, count, fout);
        if (dc) { dcr.decNs += DecCenNs() - t0; ++dcr.decN; }
        SetPendingPosMtxMax(g_lastPosMtxMax);
        static const bool check = diag::Int("GXV_FOLDAHEAD", 1) == 2;
        if (check) FoldAheadCheck(p, fp, src, count, fout);
        if (__builtin_expect(dc, 0)) {
            // (the vertices are copied for the census's copy-out timing before EmitDraw may change the arena)
            dcr.mask = decodeMask;
            DecCenHold(dcr, fout, count * fp.layout.stride);
            const uint64_t t1 = DecCenNs();
            const bool folded = EmitDraw(prim, decodeMask, count, p.layoutMask);
            dcr.emitNs += DecCenNs() - t1; ++dcr.emitN;
            if (folded) { DecCenNote(p, prim, src, count, origin, dcr); return; }
        } else if (EmitDraw(prim, decodeMask, count, p.layoutMask)) {
            return;
        }
        // (it did not fold after all: the arena is back as it was)
        ++g_foldRedo;
        if (check) VGX_LOG_N(50, "foldahead: a guessed fold did not fold (mask %03X, %u vertices)\n", p.layoutMask, count);
    }
    uint8_t* out = BeginVertices(p.layoutMask, count);
    if (!out) return;
    {
        workload::FeScope feDecode(workload::FeDecode);
        if (dc) dcr.out = nullptr; // (a guessed fold that did not fold: its vertices are not these)
        const uint64_t t0 = dc ? DecCenNs() : 0;
        // (GXV_FE_PARALLEL, vgx_fepar.h: the decode may go to the finishing thread; the highest matrix row, which the draw's resolution
        // needs now, is read from the stream here, as the decode would leave it)
        if (!g_fxMemo.on && fepar::Want(p, count)) { fepar::Defer(p, src, count, out); g_lastPosMtxMax = PosMtxMaxOf(p, src, count); }
        else { DecodeForDraw(p, src, count, out); if (dc) dcr.out = out; }
        if (dc) { dcr.decNs += DecCenNs() - t0; ++dcr.decN; }
    }
    SetPendingPosMtxMax(g_lastPosMtxMax);
    workload::FeScope feEmit(workload::FeEmit);
    if (__builtin_expect(dc, 0)) {
        // (the census times its copy-out on a copy taken here: EmitDraw may move or shorten the arena)
        dcr.mask = p.layoutMask;
        if (dcr.out) DecCenHold(dcr, dcr.out, count * p.layout.stride);
        const uint64_t t1 = DecCenNs();
        EmitDraw(prim, p.layoutMask, count);
        dcr.emitNs += DecCenNs() - t1; ++dcr.emitN;
        DecCenNote(p, prim, src, count, origin, dcr);
        return;
    }
    EmitDraw(prim, p.layoutMask, count);
}

void DrawStream(uint8_t prim, int fmt, const uint8_t* src, uint32_t count, uint32_t avail) { DrawStreamPlan(prim, fmt, GetPlan(fmt), src, count, avail, 0); }

void DrawDecoded(uint8_t prim, uint32_t layoutMask, const uint8_t* verts, uint32_t count) {
    if (count == 0) return;
    uint8_t* out = BeginVertices(layoutMask, count);
    if (!out) return; // (BeginVertices' guard refused the arena)
    std::memcpy(out, verts, static_cast<size_t>(count) * MakeLayout(layoutMask).stride);
    EmitDraw(prim, layoutMask, count);
}

// ------------------------------------------------------- command streams
namespace {
// The game's GX library writes the same BP registers again for every material, with the values they already hold. Writing
// such a value changes nothing (WriteBP's tail: "if it differs, store and mark") except for the registers listed here, which
// act on every write: the mask for the next write, the draw-done / token / copy triggers and the palette load.
struct BpEffectTable {
    uint8_t t[256];
    constexpr BpEffectTable() : t() {
        for (int i = 0; i < 256; ++i) t[i] = (i == 0xFE || i == 0x45 || i == 0x47 || i == 0x48 || i == 0x52 || i == 0x65 || i == 0xFF) ? 1 : 0; // (0xFF: the port's marker)
    }
};
constexpr BpEffectTable kBpEffect;
// Registers whose write is WriteBP's plain tail ("if it differs, store and mark"): not the mask, the triggers above, the TEV
// colour halves or the texture-unit registers.
// (texture-unit registers, as WriteBP's texture path has them: tex = the texmap bit for mode0, mode1, image0, image3 and the
// palette register, whose change makes the binding stale; 0 for image1 / image2 and the gaps, which change only bp[])
struct BpPlainTable {
    uint8_t t[256], tex[256];
    constexpr BpPlainTable() : t(), tex() {
        for (int i = 0; i < 256; ++i) {
            t[i] = (i == 0xFE || i == 0x45 || i == 0x47 || i == 0x48 || i == 0x52 || i == 0x65 || i == 0xFF || (i >= 0xE0 && i <= 0xE7) || (i >= 0x80 && i <= 0xBB)) ? 0 : 1;
            if (i >= 0x80 && i <= 0xBB) {
                const int base = i >= 0xA0 ? 0xA0 : 0x80;
                const int kind = (i - base) >> 2;
                if (kind != 3 && kind != 4 && kind != 7) tex[i] = static_cast<uint8_t>(1u << ((i & 3) + (base == 0xA0 ? 4 : 0)));
            }
        }
    }
};
constexpr BpPlainTable kBpPlain;
// WriteCP with the registers of every shape inline: array bases and strides, the matrix indices.
inline __attribute__((always_inline)) void WriteCPInline(uint8_t reg, uint32_t v) {
    if (reg >= 0xA0 && reg <= 0xBF) {
        const int n = reg & 0xF;
        uint32_t& d = (reg & 0x10) ? g.cpStride[n] : g.cpBase[n];
        const uint32_t nv = (reg & 0x10) ? (v & 0xFF) : v;
        if (d != nv) { d = nv; g.cpArrayStale |= static_cast<uint16_t>(1u << n); }
    } else if (reg == 0x30) {
        if (g.matIdxA != v) { g.matIdxA = v; g.xfMtxDirty = true; }
    } else if (reg == 0x40) {
        if (g.matIdxB != v) { g.matIdxB = v; g.xfMtxDirty = true; }
    } else {
        WriteCP(reg, v);
    }
}
inline __attribute__((always_inline)) bool BpWriteIsNoop(uint32_t rv) {
    const uint32_t reg = rv >> 24;
    return !kBpEffect.t[reg] && g.bpMask == 0x00FFFFFFu && g.bp[reg] == (rv & 0x00FFFFFFu);
}
// GXV_NOOP_FILTER (fedl25; default 0). The top-level stream's register writes are about 70 % repeats of what the register holds (the stream
// census of the console: 11.4k writes a picture, 8.1k of them changing nothing; BP, CP array bases / strides / matrix indices, XF matrix
// memory and registers). BPFAST's burst loop took only a run of BP no-ops; every other repeat went round the whole command loop (the look-ahead,
// two-core and bracket hooks, the opcode chain) and then WriteCPInline / WriteXFBE's own tests. With the switch on, one loop takes every
// repeat of those kinds, whichever follows which, with the tests of the write functions themselves (a pure "same value": the functions store
// and mark only a change, so nothing else is left out).
//   1  on;  2  verify: nothing is left out; before each command the loop would have skipped, the dirty flags, keys and registers it may touch
//      are summed, the command is made the normal way, and the sums are compared ("noopfilter: N checked, M mismatching").
// Left alone (the normal path): the census forms, the second worker's clone, GXV_FE_TWOCORE's player, J3D-native hooks that note XF writes.
int g_noopMode = -1;
int NoopModeSlow() {
    long v = EnvInt("GXV_NOOP_FILTER", 0);
    if (v != 1 && v != 2) v = 0;
    if (v == 2 && !diag::kEnabled) v = 1;
    g_noopMode = static_cast<int>(v);
    return g_noopMode;
}
inline int NoopMode() {
#if defined(VGX_CLONE_B)
    return 0;
#else
    return __builtin_expect(g_noopMode >= 0, 1) ? g_noopMode : NoopModeSlow();
#endif
}
uint64_t g_noopChecked = 0, g_noopBad = 0;
// CP writes WriteCPInline makes without a trace when the register holds the value (array bases, strides, the two matrix-index words).
inline bool CpIsNoop(uint32_t reg, uint32_t v) {
    if (reg >= 0xA0 && reg <= 0xBF) {
        const uint32_t n = reg & 0xF;
        return (reg & 0x10) ? g.cpStride[n] == (v & 0xFF) : g.cpBase[n] == v;
    }
    if (reg == 0x30) return g.matIdxA == v;
    if (reg == 0x40) return g.matIdxB == v;
    return false;
}
// n host words at d against n big-endian words at be: all equal (SwapStoreDiff's byte-swap and compare, 16 bytes at a time, without the store)
inline bool SameBeRaw(const uint8_t* d, const uint8_t* be, uint32_t n) {
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    uint32x4_t acc = vdupq_n_u32(0);
    for (; n >= 4; n -= 4, d += 16, be += 16) acc = vorrq_u32(acc, vreinterpretq_u32_u8(veorq_u8(vrev32q_u8(vld1q_u8(be)), vld1q_u8(d))));
    const uint32x2_t h = vorr_u32(vget_low_u32(acc), vget_high_u32(acc));
    if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) != 0) return false;
#endif
    for (; n > 0; --n, d += 4, be += 4) {
        uint32_t o;
        std::memcpy(&o, d, 4);
        if (o != be32(be)) return false;
    }
    return true;
}
inline bool SameBeWords(const uint32_t* d, const uint8_t* be, uint32_t n) { return SameBeRaw(reinterpret_cast<const uint8_t*>(d), be, n); }
inline bool SameBeFloats(const float* d, const uint8_t* be, uint32_t n) { return SameBeRaw(reinterpret_cast<const uint8_t*>(d), be, n); }
// An XF load (addr, n words, big-endian) whose every word is what WriteXFWord would find there: matrix memories, light memory, registers
// (the two matrix-index registers also against CP's copy, as WriteXFWord keeps them).
inline bool XfIsNoop(uint32_t addr, uint32_t n, const uint8_t* be) {
    if (addr + n <= 0x100) return SameBeFloats(&g.xfPos[0][0] + addr, be, n);
    if (addr >= 0x400 && addr + n <= 0x460) return SameBeFloats(&g.xfNrm[0][0] + (addr - 0x400), be, n);
    if (addr >= 0x500 && addr + n <= 0x600) return SameBeFloats(&g.xfPost[0][0] + (addr - 0x500), be, n);
    if (addr >= 0x600 && addr + n <= 0x680) return SameBeWords(&g.xfLight[0][0] + (addr - 0x600), be, n);
    if (addr >= 0x1000 && addr + n <= 0x1058) {
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t a = addr + i, w = be32(be + i * 4u);
            if (g.xfReg[a - 0x1000] != w) return false;
            if (a == 0x1018 && g.matIdxA != w) return false;
            if (a == 0x1019 && g.matIdxB != w) return false;
        }
        return true;
    }
    return false;
}
#if defined(GXV_NOOP_MUTANT) && GXV_NOOP_MUTANT == 1
// (test-only negative control, -DGXV_NOOP_MUTANT=1: an XF load counts as a repeat when its FIRST word is the register's: the pictures must change)
#define XF_IS_NOOP(a, n, be) (((a) + (n) <= 0x100 && SameBeFloats(&g.xfPos[0][0] + (a), (be), 1)) ? true : XfIsNoop((a), (n), (be)))
#else
#define XF_IS_NOOP(a, n, be) XfIsNoop((a), (n), (be))
#endif
// The command at p (an opcode byte, `end - p` bytes there): its size when it is a BP / CP / XF write that changes nothing, else 0.
inline uint32_t NoopCmdLen(const uint8_t* p, const uint8_t* end) {
    const uint8_t c = p[0];
    if (c == 0x61) {
        if (end - p < 5) return 0;
        return BpWriteIsNoop(be32(p + 1)) ? 5u : 0u;
    }
    if (c == 0x08) {
        if (end - p < 6) return 0;
        return CpIsNoop(p[1], be32(p + 2)) ? 6u : 0u;
    }
    if (c == 0x10) {
        if (end - p < 5) return 0;
        const uint32_t n = static_cast<uint32_t>(be16(p + 1) & 0xF) + 1u, addr = be16(p + 3);
        if (static_cast<uint32_t>(end - p) < 5u + n * 4u) return 0;
        return XF_IS_NOOP(addr, n, p + 5) ? 5u + n * 4u : 0u;
    }
    return 0;
}
// The loop (GXV_NOOP_FILTER=1): from p, the repeats in a row; returns where the first command that is not one starts.
inline const uint8_t* NoopRun(const uint8_t* p, const uint8_t* end, uint32_t& stateCmds) {
    while (end - p >= 5) {
        const uint32_t len = NoopCmdLen(p, end);
        if (!len) break;
        p += len;
        ++stateCmds;
    }
    return p;
}
// GXV_FE_STATEFAST (fe2-26; default 0). The top-level stream is ~11,700 state commands a picture (BP, CP, XF, indexed matrix loads), most of them
// short; every one went round ProcessStreamT's whole command loop: the two-core, census, J3D-native and look-ahead tests, the opcode chain, then the
// write function with its own repeat test. Measured on the host (tests/vgx_fe_work.cpp FEW_TOP, callgrind): 46-76 instructions for a repeated CP / BP write,
// 120-230 for a repeated XF register write, ~100 more for a changed one than the write function itself costs. One loop here takes a run of such commands
// with those tests made once at its entry, and the same write functions as the general loop makes (WriteBP, WriteCPInline, WriteXFBE, WriteXF, the
// indexed-load path line for line); a repeat is recognised with NoopCmdLen's tests (the ones GXV_NOOP_FILTER verified) and left out, a change goes to
// the write function as before. It stops (returns the command's start) at the first command it does not take: a BP register with an effect (mask,
// tokens, copies, palette, markers), CP/XF writes cut by the end of the buffer, any other opcode.
//   0 off, 1 on, 2 verify (every command the loop would leave out is made the normal way and the state digest compared before and after:
//   "statefast: N checked, M mismatching", must be 0; what the loop sends to the write functions is the same call the general loop makes).
// Entered only where the general loop has nothing of its own to do for these commands: not the census form, not GXV_BPFAST=0, not the second
// worker or the two-core player, not with the J3D-native hooks, not with GXV_NOOP_FILTER (it has its own loop).
int g_stateFast = -1;
int StateFastSlow() {
    long v = EnvInt("GXV_FE_STATEFAST", 0);
    if (v != 1 && v != 2) v = 0;
    if (v == 2 && !diag::kEnabled) v = 1;
    g_stateFast = static_cast<int>(v);
    return g_stateFast;
}
inline int StateFastMode() {
#if defined(VGX_CLONE_B)
    return 0;
#else
    return __builtin_expect(g_stateFast >= 0, 1) ? g_stateFast : StateFastSlow();
#endif
}
uint64_t g_sfChecked = 0, g_sfBad = 0, g_sfCmds = 0, g_sfRuns = 0;
uint64_t NoopDigest();
// XfIsNoop with the registers first (the commonest repeat in the top-level stream)
inline bool StateXfSame(uint32_t addr, uint32_t cnt, const uint8_t* be) {
    if (addr >= 0x1000 && addr + cnt <= 0x1058) {
#if defined(GXV_STATEFAST_MUTANT) && GXV_STATEFAST_MUTANT == 1
        // (test-only negative control, -DGXV_STATEFAST_MUTANT=1: only the FIRST word of an XF register write is compared: the verify mode must see it)
        if (cnt > 1) return g.xfReg[addr - 0x1000] == be32(be);
#endif
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint32_t w = be32(be + i * 4u), a = addr + i;
            if (g.xfReg[a - 0x1000] != w || (a == 0x1018 && g.matIdxA != w) || (a == 0x1019 && g.matIdxB != w)) return false;
        }
        return true;
    }
    return XF_IS_NOOP(addr, cnt, be);
}
template <bool kVerify>
__attribute__((noinline)) const uint8_t* StateRun(const uint8_t* p, const uint8_t* const end, uint32_t& stateCmds) {
    uint32_t n = 0;
    while (end - p >= 6) {
        const uint8_t cmd = p[0];
        if (cmd == 0x61) {
            const uint32_t rv = be32(p + 1);
            const uint32_t reg = rv >> 24;
            if (kBpEffect.t[reg]) break; // (the mask, tokens, copies, palette loads, the port's markers: the general loop)
            if (kVerify) {
                if (g.bpMask == 0x00FFFFFFu && g.bp[reg] == (rv & 0x00FFFFFFu)) {
                    const uint64_t before = NoopDigest();
                    WriteBP(rv);
                    ++g_sfChecked;
                    if (NoopDigest() != before) { ++g_sfBad; VGX_LOG_N(16, "statefast: MISMATCH (BP register 0x%02X)\n", reg); }
                    if ((g_sfChecked & 0xFFFFFu) == 0) VGX_LOG_N(200, "statefast: %llu checked, %llu mismatching\n", static_cast<unsigned long long>(g_sfChecked), static_cast<unsigned long long>(g_sfBad));
                    p += 5; ++n; continue;
                }
            } else if (g.bp[reg] == (rv & 0x00FFFFFFu) && g.bpMask == 0x00FFFFFFu) {
                p += 5; ++n; continue;
            }
            p += 5;
            if (kBpPlain.t[reg] && g.bpMask == 0x00FFFFFFu) { // (WriteBP's plain tail: store, mark; the mask is the default and the register has no side effect)
                const uint32_t ov = g.bp[reg], nv = rv & 0x00FFFFFFu;
                g.bp[reg] = nv;
                MarkBP(static_cast<uint8_t>(reg), ov, nv);
            } else {
                WriteBP(rv);
            }
            ++n;
        } else if (cmd == 0x08) {
            const uint8_t reg = p[1];
            const uint32_t v = be32(p + 2);
            p += 6;
            if (kVerify && CpIsNoop(reg, v)) {
                const uint64_t before = NoopDigest();
                WriteCPInline(reg, v);
                ++g_sfChecked;
                if (NoopDigest() != before) { ++g_sfBad; VGX_LOG_N(16, "statefast: MISMATCH (CP register 0x%02X)\n", reg); }
            } else {
                WriteCPInline(reg, v);
            }
            ++n;
        } else if (cmd == 0x10) {
            const uint32_t cnt = static_cast<uint32_t>(be16(p + 1) & 0xF) + 1u;
            const uint32_t addr = be16(p + 3);
            const uint32_t len = 5u + cnt * 4u;
            if (static_cast<uint32_t>(end - p) < len) break; // (cut by the end of the buffer: the general loop reports it)
            if (StateXfSame(addr, cnt, p + 5)) {
                if (kVerify) {
                    const uint64_t before = NoopDigest();
                    WriteXFBE(addr, cnt, p + 5);
                    ++g_sfChecked;
                    if (NoopDigest() != before) { ++g_sfBad; VGX_LOG_N(16, "statefast: MISMATCH (XF address 0x%X)\n", addr); }
                }
            } else {
                WriteXFBE(addr, cnt, p + 5);
            }
            p += len;
            ++n;
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            const uint32_t index = be16(p + 1);
            const uint32_t v = be16(p + 3);
            p += 5;
            const uint32_t cnt = (v >> 12) + 1u, addr = v & 0xFFF;
            const int which = (cmd - 0x20) >> 3;
            SyncArrays(static_cast<uint16_t>(1u << (12 + which)));
            bool le = false;
            const uint8_t* src = ArrayElem(A_POSMTXARRAY + which, index, cnt * 4u, le);
            if (src != kZero) {
                if (le) { // (matrices the game's code stored: host words)
                    uint32_t w[16];
                    std::memcpy(w, src, cnt * 4u);
                    WriteXF(addr, cnt, w);
                } else {
                    WriteXFBE(addr, cnt, src);
                }
            }
            ++n;
        } else {
            break;
        }
    }
    stateCmds += n;
    g_sfCmds += n;
    ++g_sfRuns;
    return p;
}
// A digest of what a write of the three kinds may touch: the dirty flags, the keys, the registers.
uint64_t NoopDigest() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    uint32_t w[3];
    std::memcpy(w, &g.bpDirty, 12);
    mix(w[0]); mix(w[1]); mix(w[2]);
    mix(g.bpMask); mix(g.cpArrayStale); mix(g.bpTexStale); mix(g.texSlotDirty); mix(g.tevColDirty); mix(g.xfPosRowsDirty); mix(g.layoutDirty);
    mix(g.vcdGen); mix(g.keyHash); mix(g.matIdxA); mix(g.matIdxB); mix(g.ltCos1); mix(g.ltDist1);
    for (uint32_t r : g.bp) mix(r);
    for (uint32_t r : g.cpBase) mix(r);
    for (uint32_t r : g.cpStride) mix(r);
    for (uint32_t r : g.xfReg) mix(r);
    const uint32_t* q = reinterpret_cast<const uint32_t*>(&g.xfPos[0][0]);
    for (uint32_t i = 0; i < 256; ++i) mix(q[i]);
    q = reinterpret_cast<const uint32_t*>(&g.xfNrm[0][0]);
    for (uint32_t i = 0; i < 96; ++i) mix(q[i]);
    q = reinterpret_cast<const uint32_t*>(&g.xfPost[0][0]);
    for (uint32_t i = 0; i < 256; ++i) mix(q[i]);
    for (const auto& row : g.xfLight) for (uint32_t r : row) mix(r);
    return h;
}
// GXV_NOOP_FILTER=2: the command at p, if the loop would skip it, is made the normal way here (the caller then makes it again: a
// write of the value the register holds, which is what the check has just found, leaves nothing to do twice) and the digest compared.
void NoopCheck(const uint8_t* p, const uint8_t* end) {
    if (!NoopCmdLen(p, end)) return;
    const uint64_t before = NoopDigest();
    if (p[0] == 0x61) WriteBP(be32(p + 1));
    else if (p[0] == 0x08) WriteCPInline(p[1], be32(p + 2));
    else WriteXFBE(be16(p + 3), static_cast<uint32_t>(be16(p + 1) & 0xF) + 1u, p + 5);
    ++g_noopChecked;
    if (NoopDigest() != before) {
        ++g_noopBad;
        VGX_LOG_N(16, "noopfilter: MISMATCH (command 0x%02X, register/address 0x%X)\n", p[0], p[0] == 0x10 ? be16(p + 3) : p[1]);
    }
    if ((g_noopChecked & 0xFFFFFu) == 0)
        VGX_LOG_N(200, "noopfilter: %llu checked, %llu mismatching\n", static_cast<unsigned long long>(g_noopChecked), static_cast<unsigned long long>(g_noopBad));
}
// GXV_BPFAST=0: every BP write goes through WriteBP; 2: the skipped ones too, and the state they might change is compared (logged).
void BpCheckNoop(uint32_t rv) {
    static uint64_t checked = 0, bad = 0;
    struct Snap {
        bool d0, d1, d2, d3; uint8_t s0, s1, s2; uint32_t mask, val;
        bool operator==(const Snap& o) const { return d0 == o.d0 && d1 == o.d1 && d2 == o.d2 && d3 == o.d3 && s0 == o.s0 && s1 == o.s1 && s2 == o.s2 && mask == o.mask && val == o.val; }
    };
    const uint32_t reg = rv >> 24;
    const Snap a{g.bpDirty, g.bpKeyDirty, g.bpRsDirty, g.texDirty, g.texSlotDirty, g.bpTexStale, g.tevColDirty, g.bpMask, g.bp[reg]};
    WriteBP(rv);
    const Snap b{g.bpDirty, g.bpKeyDirty, g.bpRsDirty, g.texDirty, g.texSlotDirty, g.bpTexStale, g.tevColDirty, g.bpMask, g.bp[reg]};
    ++checked;
    if (!(a == b)) {
        ++bad;
        VGX_LOG_N(16, "bpfast: MISMATCH (register 0x%02X, value 0x%06X)\n", reg, rv & 0x00FFFFFFu);
    }
    if ((checked & 0xFFFFFu) == 0)
        VGX_LOG_N(200, "bpfast: %llu writes checked, %llu mismatching\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
}
} // namespace

// GXV_GX_DEDUP=2 (gxdedup31; the game's gate: platform/src/gx/gx_dedup.inc, the decision: platform/include/port/gx_dedup_core.h): in front
// of every register write the game's =1 would leave out, an XF load of two words at 0x0C00 (an address that holds nothing, here or on the
// console): what (class << 24 | register: 0 BP, 1 CP, 2 XF with its address) and the value. The write that follows must change nothing:
// here, the register holds the value (BP: no mask pending, and the TEV colour half the selector names too); every GXV_GX_DEDUP_DIGEST-th
// probe (default 64; 1: every one) and every probe whose register this file keeps no copy of by value, the write is also made here and the
// digest of everything a register write can touch (NoopDigest) compared before / after. "gxdedup verify:" every 2^18 checks and at each of
// the first 8 that differ (a differing one: the game's shadow missed a writer, or the register is not a plain store).
#if !defined(VGX_CLONE_B)
namespace {
uint32_t g_ddWhat = 0;
bool g_ddHave = false;
uint64_t g_ddChecked = 0, g_ddBad = 0, g_ddDigests = 0, g_ddDigestBad = 0;
}
void GxDedupProbe(uint32_t a, uint32_t w) {
    if (a == 0x0C00u) { g_ddWhat = w; g_ddHave = true; return; }
    if (!g_ddHave) return;
    g_ddHave = false;
    static const uint32_t every = [] { const long n = EnvInt("GXV_GX_DEDUP_DIGEST", 64); return n < 1 ? 1u : static_cast<uint32_t>(n); }();
    const uint32_t cls = g_ddWhat >> 24, reg = g_ddWhat & 0xFFFFu;
    bool ok = false, byValue = true;
    if (cls == 0) {
        const uint32_t v = w & 0x00FFFFFFu;
        ok = reg < 0x100 && !kBpEffect.t[reg] && g.bpMask == 0x00FFFFFFu && g.bp[reg] == v && (reg < 0xE0 || reg > 0xE7 || g.tevRaw[TevRawSlot(reg, v)] == v);
    } else if (cls == 1) {
        if (reg >= 0xA0 && reg <= 0xBF) ok = (reg & 0x10) ? g.cpStride[reg & 0xF] == (w & 0xFF) : g.cpBase[reg & 0xF] == w;
        else if (reg == 0x30) ok = g.matIdxA == w;
        else if (reg == 0x40) ok = g.matIdxB == w;
        else {
            const int slot = reg < 0x100 ? CpRawSlot(static_cast<uint8_t>(reg)) : -1;
            if (slot >= 0 && ((g.cpRawValid >> slot) & 1u)) ok = g.cpRaw[slot] == w;
            else { ok = true; byValue = false; } // (not kept by value: the digest decides)
        }
    } else if (cls == 2) {
        ok = reg >= 0x1000 && reg < 0x1058 && g.xfReg[reg - 0x1000] == w && (reg != 0x1018 || g.matIdxA == w) && (reg != 0x1019 || g.matIdxB == w);
    }
    ++g_ddChecked;
    bool bad = !ok;
    if (ok && (!byValue || g_ddChecked % every == 0)) { // (the write made here, the digest compared; the stream's own write then finds nothing to do)
        const uint64_t before = NoopDigest();
        if (cls == 0) WriteBP((reg << 24) | (w & 0x00FFFFFFu));
        else if (cls == 1) WriteCP(static_cast<uint8_t>(reg), w);
        else WriteXFWord(reg, w);
        ++g_ddDigests;
        if (NoopDigest() != before) { ++g_ddDigestBad; bad = true; }
    }
    if (bad) ++g_ddBad;
    if ((bad && g_ddBad <= 8) || (g_ddChecked & 0x3FFFFu) == 0) {
        uint32_t fe = 0;
        if (cls == 0 && reg < 0x100) fe = g.bp[reg];
        else if (cls == 1 && reg >= 0xA0 && reg <= 0xBF) fe = (reg & 0x10) ? g.cpStride[reg & 0xF] : g.cpBase[reg & 0xF];
        else if (cls == 2 && reg >= 0x1000 && reg < 0x1058) fe = g.xfReg[reg - 0x1000];
        if (port_log_summary)
            port_log_summary("gxdedup verify: %llu checked, %llu differing (digests %llu, differing %llu)%s (%s %04X: front end %08X, the game's shadow %08X, mask %06X)",
                             static_cast<unsigned long long>(g_ddChecked), static_cast<unsigned long long>(g_ddBad), static_cast<unsigned long long>(g_ddDigests),
                             static_cast<unsigned long long>(g_ddDigestBad), bad ? ": MISMATCH" : "", cls == 0 ? "BP" : cls == 1 ? "CP" : "XF", reg, fe, w, g.bpMask);
    }
}
uint64_t GxDedupVerifyCounts(uint64_t* bad, uint64_t* digests) { *bad = g_ddBad; *digests = g_ddDigests; return g_ddChecked; }
#else
void GxDedupProbe(uint32_t, uint32_t) {}
#endif

#include "vgx_census.inc" // GXV_STREAM_CENSUS (lighter25): the stream by kind and source, ProcessStreamT<true>

// ------------------------------------------------------------------ look-ahead (GXV_FE_PREFETCH)
// The player is mostly waiting for memory, not computing. A picture calls about a thousand display lists, and each call looks up
// a handful of the front end's own structures that the game's and the back end's work has pushed out of the caches since the
// last picture: the state-list front and table, the list's entry, its pre-parsed operations, the geometry cache's bypass map,
// front, list and entry, the write-generation counters, then for the draw the shader-key memo, the keys and the texture memo.
// Each is a dependent load to RAM (and, outside the TLB's reach, a page-table walk first): roughly half a microsecond to a
// microsecond each on the console, which is what the per-call costs of the GXV_FE_SPLIT line (5-15 us for a few hundred
// instructions) are made of. The top-level stream says which lists come next: CALL_DL commands with their addresses, a few
// dozen bytes apart. So at each top-level CALL_DL the stream is parsed a little ahead (command sizes only, up to the next draw,
// whose size depends on the vertex format then) and the next four calls are hinted in stages, as they come nearer: four calls
// ahead the front-table slots and generation counters (CallListPrefetch stage 1), three ahead the entries those slots point at
// (2), two ahead what the entries point at (3), one ahead the operations and buffers the call will walk (4). The resolution of
// the next draw is hinted as well (ResolvePrefetch: the key-memo set once the material's registers are in, then the keys).
// Nothing here writes anything the front end reads, and a hint never faults: the stream, the state, the caches and the command
// buffers are what they are without it (GXV_FE_PREFETCH=0); only the time the loads take changes.
//   GXV_FE_PREFETCH=<n>  (read once) look n calls ahead (1-4); 0: off (the default: measured on the console, 4 Nov, Observatory
//   route, the hints cost more than they saved: front end 17.4 ms a picture with 4 against 15.7 ms without).
int FePrefetchDepth() {
    const int d = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_FE_PREFETCH", 0);
        return v <= 0 ? 0 : (v > 4 ? 4 : static_cast<int>(v));
    }()));
    return d;
}
static uint64_t g_fePfCalls = 0, g_fePfNext = 0; // top-level CALL_DLs, and those whose next call was hinted one call ahead (stage 4)
void FePrefetchCounters(uint64_t out[2]) { out[0] = g_fePfCalls; out[1] = g_fePfNext; }
namespace {
struct FeAhead {
    static constexpr int kMax = 4;
    struct Call { const uint8_t* at; const uint8_t* list; uint32_t size; int stage; };
    Call q[kMax];
    int n = 0;
    const uint8_t* scan = nullptr; // a command boundary at or after the main parse: where the look-ahead goes on
    bool blocked = false;          // `scan` is at a draw, an unknown opcode or a cut command: wait until the main parse is past it
    // At the top-level CALL_DL at `cur` (the command ends at `next`): the calls after it, up to `depth`, each hinted at the stage of its distance.
    void Step(const uint8_t* cur, const uint8_t* next, const uint8_t* end, int depth) {
        ++g_fePfCalls;
        int k = 0;
        while (k < n && q[k].at <= cur) ++k; // (this call, and nothing else, was the first)
        if (k) {
            for (int i = k; i < n; ++i) q[i - k] = q[i];
            n -= k;
        }
        if (!scan || scan < next) { // (the main parse is past where the look-ahead stopped: it goes on from here)
            scan = next;
            blocked = false;
            n = 0;
        }
        while (n < depth && !blocked && scan < end) {
            const uint8_t* s = scan;
            const uint8_t c = *s;
            if (c == 0x00) { // (as ProcessStream skips padding)
                ++s;
                while (end - s >= 4) {
                    uint32_t w;
                    std::memcpy(&w, s, 4);
                    if (w) break;
                    s += 4;
                }
                scan = s;
                continue;
            }
            size_t len;
            if (c == 0x61 || c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) len = 5;
            else if (c == 0x08) len = 6;
            else if (c == 0x40) len = 9;
            else if (c == 0x44 || c == 0x48) len = 1;
            else if (c == kRetainedOp) len = 5; // (GXV_RETAINED: the handle in front of a CALL_DL; the call itself is the next command)
            else if (c == kJ3nOp) len = 21;      // (GXV_J3DNATIVE: the bracket's five words; the call is the next command, its end marker the one after)
            else if (c == kJ3nEndOp) len = 1;
            else if (c == 0x10) {
                if (end - s < 5) { blocked = true; break; }
                len = 5u + ((be16(s + 1) & 0xFu) + 1u) * 4u;
            } else {
                blocked = true; // a draw (its size depends on the vertex format at that point) or an opcode ProcessStream resynchronises on
                break;
            }
            if (static_cast<size_t>(end - s) < len) { blocked = true; break; }
            if (c == 0x40) {
                const uint32_t addr = be32(s + 1), size = be32(s + 5);
                if (addr && size) {
                    uint32_t avail = 0;
                    const uint8_t* list = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
                    if (list && avail >= size) q[n++] = Call{s, list, size, 0};
                }
            }
            scan = s + len;
        }
        if (end - scan > 96) FePrefetch(scan + 96); // (the stream itself, ahead of the look-ahead)
        for (int i = 0; i < n; ++i) {
            const int stage = 4 - i; // distance i + 1
            if (q[i].stage >= stage) continue;
            q[i].stage = stage;
            if (stage == 4) ++g_fePfNext;
            CallListPrefetch(q[i].list, q[i].size, stage);
        }
    }
};
} // namespace

// ------------------------------------------------------------------ particle commands (GXV_FXMERGE)
// JParticle draws a particle as a few GX calls: its TEV colours (GXSetTevColor: register 0 and/or 1, the BG half three times), its
// texture matrix for some emitters, its position matrix (PNMTX0), then GXCallDisplayList of jpa_dl, one quad of 32 bytes. Each call
// was a top-level command here, and each list call went CallList -> CallListBody -> ProcessStream -> DrawStreamPlan with a decode of
// the same four vertices. With GXV_FXMERGE=1 the game's GX library gathers the calls of an emitter's particle loop (only those four
// functions; JPAResource::drawP / drawC start and end it, decomp/src/RVL_SDK/gx/GXDisplayList.c) and sends them as one port-private
// command: opcode 0x58, the payload's length (16 bits, big-endian), then items, each the same bytes the stream would have carried:
//   0x62 RA BG          a TEV colour: the two BP words GXSetTevColor writes (RA once, BG three times, as it writes them)
//   0x10 reg words..    an XF load, as the stream's XF command (reg: count - 1 << 16 | address)
//   0x40 addr size f    a display-list call, as CALL_DL, then f: the vertex count of the flush primitive GX sent in front of it
//                       (GX_MT_XF_FLUSH; an all-zero triangle strip of at most 16 vertices, which DrawStreamPlan drops), 0 none
// Each item is done here with the functions the stream's command would have reached, in the same order, so the state, the draws and
// the records are the ones the separate commands give. What is left out is only the walk around them: the top-level parse, and for a
// call of a list CallList would parse at once (FxTinyDraw: one draw command, then padding) its layers and, from the second such call
// of the command on, the decode (DecodeForDraw). Any other list goes through CallList as before.
namespace {
constexpr uint8_t kFxMergeOp = 0x58;
uint64_t g_fxCmds = 0, g_fxItems = 0, g_fxCalls = 0, g_fxShort = 0, g_fxThrough = 0;
// A BP write as ProcessStream makes it (GXV_BPFAST: a write that changes nothing is skipped; =2 checks the skip).
inline void FxBP(uint32_t v, int bpFast) {
    if (bpFast != 0 && BpWriteIsNoop(v)) {
        if (diag::kEnabled && bpFast == 2) BpCheckNoop(v);
        return;
    }
    WriteBP(v);
}
// (GXV_DLTRACE: ProcessStream traces every list of that frame; such a frame keeps the separate path)
inline bool FxDlTraceNow() {
    static const long dlTrace = diag::Int("GXV_DLTRACE", -1);
    return dlTrace >= 0 && static_cast<uint64_t>(dlTrace) == CurrentFrame();
}
// CallList(list, nbytes) for a list that is one draw command and padding, when CallList would parse it at once: the same calls
// CallListBody and ProcessStream make for it, in their order. False (nothing done): any other list.
bool FxTinyDraw(const uint8_t* list, uint32_t nbytes) {
    if (nbytes < 3 || !(list[0] & 0x80) || FxDlTraceNow() || !CallListIsTinyStream(list, nbytes)) return false;
    // (CallListBody's tiny branch)
    SyncVertexArrays();
    const uint8_t cmd = list[0];
    const uint32_t count = be16(list + 1);
    const Plan& plan = GetPlan(cmd & 7);
    const uint32_t vsize = plan.streamSize;
    const uint32_t avail = nbytes - 3u;
    const uint64_t used = static_cast<uint64_t>(count) * vsize;
    if (used > avail) return false;
    {
        // (more commands after the draw: ProcessStream's way)
        uint32_t i = 3u + static_cast<uint32_t>(used), any = 0;
        for (; i + 4u <= nbytes; i += 4u) {
            uint32_t w;
            std::memcpy(&w, list + i, 4);
            any |= w;
        }
        for (; i < nbytes; ++i) any |= list[i];
        if (any) return false;
    }
    // (ProcessStream on the list: its statistics, the draw, the padding)
    ++stats::Cur().streams;
    stats::Cur().streamBytes += nbytes;
    if (vsize != 0 && count != 0) {
        DrawStreamPlan(cmd & 0xF8, cmd & 7, plan, list + 3, count, avail, 2);
        ++stats::Cur().primsDL;
        stats::Cur().vertsDL += count;
    }
    return true;
}
// A CALL_DL item, as ProcessStream's CALL_DL (top level) with the flush primitive in front of it.
void FxCall(uint32_t addr, uint32_t size, uint32_t flushVerts, bool memo) {
    if (flushVerts) {
        // (ProcessStream on the all-zero strip: its plan, then DrawStreamPlan, which drops it after the array sync)
        const Plan& fp = GetPlan(0);
        if (fp.streamSize != 0) {
            {
                workload::FeScope feDraw(workload::FeDraw);
                SyncVertexArrays();
            }
            ++stats::Cur().primsDL;
            stats::Cur().vertsDL += flushVerts;
        }
    }
    if (!addr || !size) return;
    uint32_t avail = 0;
    const uint8_t* list = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
    if (!list || avail < size) {
        VGX_LOG_N(16, "CALL_DL %08X + %u: not mapped\n", addr, size);
        return;
    }
    workload::FeScope feList(workload::FeList);
    ++g_fxCalls;
    g_fxMemo.on = memo;
    const bool done = FxTinyDraw(list, size);
    g_fxMemo.on = false;
    if (done) {
        ++g_fxShort;
        return;
    }
    g_fxMemo.valid = false; // (the list may change what a decode reads)
    ++g_fxThrough;
    g_tlSite = 2; // (GXV_TINYLIST_CENSUS: the call site)
    CallList(list, size);
    g_tlSite = 0;
}
// One command's payload (complete: ProcessStream hands it over only whole).
void FxMergeRun(const uint8_t* p, uint32_t n) {
    workload::FeScope fe(workload::FeFxMerge);
    ++g_fxCmds;
    const uint8_t* const end = p + n;
    const int bpFastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1))));
    const int bpFast = !diag::kEnabled && bpFastMode == 2 ? 1 : bpFastMode;
    // (the decode is kept from the second call on: worth it from two calls of the command, the first decoding into the memo)
    uint32_t calls = 0;
    for (const uint8_t* q = p; q < end && calls < 2;) {
        const uint8_t t = *q;
        if (t == 0x62) q += 9;
        else if (t == 0x10 && end - q >= 5) q += 5u + ((be16(q + 1) & 0xFu) + 1u) * 4u;
        else if (t == 0x40) { q += 10; ++calls; }
        else break;
    }
    const bool memo = calls >= 2;
    g_fxMemo.valid = false;
    while (p < end) {
        const uint8_t t = *p;
        ++g_fxItems;
        if (t == 0x62 && end - p >= 9) {
            const uint32_t ra = be32(p + 1), bg = be32(p + 5);
            FxBP(ra, bpFast);
            FxBP(bg, bpFast);
            FxBP(bg, bpFast);
            FxBP(bg, bpFast);
            p += 9;
        } else if (t == 0x10 && end - p >= 5) {
            const uint32_t cnt = (be16(p + 1) & 0xFu) + 1u, addr = be16(p + 3);
            if (static_cast<uint32_t>(end - p) < 5u + cnt * 4u) break;
            WriteXFBE(addr, cnt, p + 5);
            if (addr + cnt > 0x100) g_fxMemo.valid = false; // (only matrix memory below 0x100 is known to leave a decode as it was)
            p += 5u + cnt * 4u;
        } else if (t == 0x40 && end - p >= 10) {
            FxCall(be32(p + 1), be32(p + 5), p[9], memo);
            p += 10;
        } else {
            VGX_LOG_N(8, "fxmerge: unknown item 0x%02X (%u bytes left of the command)\n", t, static_cast<unsigned>(end - p));
            break;
        }
    }
    g_fxMemo.valid = false;
}
} // namespace
void FxMergeCounters(uint64_t out[6]) {
    out[0] = g_fxCmds; out[1] = g_fxItems; out[2] = g_fxCalls; out[3] = g_fxShort; out[4] = g_fxMemoHits; out[5] = g_fxThrough;
}

// GXV_J3DNATIVE (vgx_j3native.inc): the bracketed call and the census hooks of the top-level stream
void J3nCall(const uint8_t* list, uint32_t nbytes, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl);
void J3nNoteBp(uint32_t v);
void J3nNoteXf(uint32_t addr, uint32_t n, const uint8_t* be);
void J3nNoteDraw();
void J3nNoteFx();
void J3nNoteList(uint32_t nbytes);
void J3nNoteShape();
// GXV_J3DNATIVE=4 / 5 (vgx_j3native2.inc): stage 2's chains. g_j3n2On: a chain is active (recording, verify or template mode).
extern bool g_j3n2On;
void J3n2End(int reason);
void J3n2Plain(const uint8_t* list, uint32_t nbytes);
void J3n2Shape(const uint8_t* list, uint32_t nbytes, uint32_t handle);
void J3n2NoteBp(uint32_t rv);
void J3n2NoteXf(uint32_t addr, uint32_t n);
// What a chain may hold (anything else ends it before it is made): every command but a particle command (0x58) and an unknown opcode. List
// calls are decided at the call (a compiled state list is a token; anything else ends the chain), BP and XF register writes are noted
// (J3n2NoteBp / J3n2NoteXf: what they change may differ from the recording), draws read the register file, which is real.
static inline bool J3n2CmdOk(uint8_t cmd) {
    if (cmd & 0x80) return true;
    switch (cmd) {
    case 0x00: case 0x08: case 0x10: case 0x20: case 0x28: case 0x30: case 0x38: case 0x40: case 0x44: case 0x48: case 0x61: case kRetainedOp: case kJ3nOp:
    case kJ3nEndOp:
        return true;
    default:
        return false;
    }
}

// GXV_FEPLAN=2 (feplan31, the verify of a mixed list's plan, see "mixed display lists" below): while set, every draw a list's parse makes
// is recorded with the register state before and after it (the parse that the plan is then compared with). Null otherwise: one test a draw.
struct FpRec;
static FpRec* g_fpRec = nullptr;
static void FpRecBefore(uint8_t cmd, const uint8_t* src, uint32_t count, uint32_t avail);
static void FpRecAfter();

// kC (GXV_STREAM_CENSUS, vgx_census.inc): a counted picture's top-level stream, with the census's tests and counts around the same calls.
// cont: the rest of a call that switched to the other form at a census BEGIN marker or at the counted picture's display copy (the call's
// statistics were taken by its first part).
template <bool kC>
static uint32_t ProcessStreamT(const uint8_t* p, uint32_t nbytes, bool inList, bool cont) {
    workload::FeScope feStream(inList ? ~0u : workload::FeStream);
    const uint8_t* const end = p + nbytes;
    if (!cont) {
        ++stats::Cur().streams;
        stats::Cur().streamBytes += nbytes;
    }
    const uint8_t* switchAt = nullptr; // (GXV_STREAM_CENSUS: the census turned on / off at this command boundary)
    if constexpr (kC) sc::ParseBegin();
    sc::Kind ck = sc::kOther; // (kC: the kind of the command being parsed, counted at the next command's start or at the end)
    uint32_t cverts = 0;
    const uint8_t* cstart = nullptr;
    // GXV_DLTRACE=<frame>: one line per command stream of that frame.
    static const long dlTrace = diag::Int("GXV_DLTRACE", -1);
    const bool traceThis = dlTrace >= 0 && static_cast<uint64_t>(dlTrace) == CurrentFrame();
    const uint8_t* const start = p;
    const uint32_t verts0 = stats::Cur().vertsDL, prims0 = stats::Cur().primsDL;
    uint32_t stateCmds = 0;
    struct TraceOut {
        bool on; const uint8_t* start; uint32_t nbytes, verts0, prims0; uint32_t* stateCmds;
        ~TraceOut() {
            if (!on) return;
            uint32_t idxMask = 0;
            for (int a = 9; a <= 20; ++a) if (g.vcd[a] >= 2) idxMask |= 1u << a;
            std::fprintf(stderr, "[dl] %p %u bytes: %u prims, %u verts, %u state cmds, indexed attrs 0x%X, pos array %p stride %u, pnmtxidx %u\n", (const void*)start, nbytes,
                         stats::Cur().primsDL - prims0, stats::Cur().vertsDL - verts0, *stateCmds, idxMask, (const void*)g.arrays[9].data, g.arrays[9].stride, g.vcd[0]);
        }
    } traceOut{traceThis, start, nbytes, verts0, prims0, &stateCmds};
    // A command that the end of the buffer cuts short is not consumed: `cut` is its first byte.
    const uint8_t* cut = nullptr;
    // GXV_FE_PREFETCH: the look-ahead of the top-level stream (lists are called from it only)
    const int pfDepth = inList ? 0 : FePrefetchDepth();
    FeAhead ahead;
    const int bpFastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1))));
    const int bpFast = !diag::kEnabled && bpFastMode == 2 ? 1 : bpFastMode;
    // GXV_NOOP_FILTER: off in the census form, the second worker's clone, the two-core player, and while the J3D-native hooks watch the writes
    int noopMode = kC ? 0 : NoopMode();
#if !defined(VGX_CLONE_B)
    if (noopMode != 0 && (vgx2_mode != 0 || J3nMode() != 0)) noopMode = 0;
#endif
    // GXV_FE_SPLIT: the runs of top-level state commands are timed as scopes of the slot FeTopState (one scope per run, not per command)
    const bool splitRuns = !inList && workload::FeSplitMode() != 0;
    bool runOpen = false, runTimed = false;
    int64_t runT = 0;
    auto runClose = [&]() {
        if (runTimed) {
            workload::FeNs[workload::FeTopState].fetch_add(static_cast<uint64_t>(port_host_time_ns() - runT), std::memory_order_relaxed);
            workload::FeNTimed[workload::FeTopState].fetch_add(1, std::memory_order_relaxed);
        }
        workload::FeN[workload::FeTopState].fetch_add(1, std::memory_order_relaxed);
        runOpen = false;
    };
    const uint8_t* jumped = nullptr; // (GXV_FE_TWOCORE: the player took a segment the second worker made and goes on at its end)
#if !defined(VGX_CLONE_B)
    if (!inList && __builtin_expect(vgx2_mode != 0, 0)) two::Published(p, end);
#endif
    // GXV_FE_STATEFAST: StateRun takes the runs of state commands (the tests it replaces are made here once)
    int stateFast = 0;
#if !defined(VGX_CLONE_B)
    if (!kC && noopMode == 0 && bpFast == 1) {
        stateFast = StateFastMode();
        if (stateFast != 0 && (vgx2_mode != 0 || J3nMode() != 0)) stateFast = 0;
    }
#endif
    while (p < end) {
        if (stateFast != 0) {
            const uint8_t c0 = *p;
            if (c0 == 0x61 || c0 == 0x08 || c0 == 0x10 || c0 == 0x20 || c0 == 0x28 || c0 == 0x30 || c0 == 0x38) {
                if (__builtin_expect(splitRuns, 0) && !runOpen) { // (GXV_FE_SPLIT: the run's scope, opened as the general loop opens it)
                    runOpen = true;
                    runTimed = workload::FeTimedScope();
                    runT = runTimed ? port_host_time_ns() : 0;
                }
                p = stateFast == 2 ? StateRun<true>(p, end, stateCmds) : StateRun<false>(p, end, stateCmds);
                if (p >= end) break;
            }
        }
        const uint8_t* const cmdStart = p;
        if constexpr (kC) {
            if (cstart) sc::Cmd(ck, static_cast<uint32_t>(cmdStart - cstart), cverts);
            cstart = cmdStart; ck = sc::kOther; cverts = 0;
        }
#if defined(VGX_CLONE_B)
        // (GXV_FE_TWOCORE, the second worker: its state machine, before every top-level command; vgx_twocore_b.cpp)
        if (!inList && vgx2_dbg && two::g_prescan) { vgx2_dbg_b.push_back(static_cast<uint64_t>(cmdStart - vgx2_pic_start)); vgx2_dbg_b.push_back(vgx2_b_regs_hash()); }
        if (!inList && two::Before(cmdStart, end)) { cut = cmdStart; break; }
#else
        if (!inList && __builtin_expect(vgx2_mode != 0, 0)) {
            // (GXV_FE_TWOCORE, vgx_twocore.cpp: a picture's first command; a segment the second worker made starts here)
            if (two::g_needStart) two::StartPicture(cmdStart);
            if (vgx2_dbg) { vgx2_dbg_a.push_back(static_cast<uint64_t>(cmdStart - vgx2_pic_start)); vgx2_dbg_a.push_back(vgx2_a_regs_hash()); }
            vgx2_apos.store(cmdStart, std::memory_order_relaxed);
            if (two::g_verifyAt == cmdStart) two::VerifyAt(cmdStart);
            if (const uint8_t* const sa = two::StopAt()) {
                if (cmdStart == sa) {
                    const uint8_t* const np = two::Arrive(cmdStart);
                    if (np != cmdStart) {
                        p = np;
                        if (p >= end) { jumped = p; break; }
                        continue;
                    }
                } else if (sa < cmdStart) {
                    two::Missed();
                }
            }
        }
#endif
        const uint8_t cmd = *p++;
        if (__builtin_expect(g_j3n2On, 0) && !inList && !J3n2CmdOk(cmd)) J3n2End(0); // (GXV_J3DNATIVE=4 / 5: before the command)
        if (__builtin_expect(splitRuns, 0)) {
            const bool st = cmd == 0x61 || cmd == 0x08 || cmd == 0x10 || cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38;
            if (st && !runOpen) {
                runOpen = true;
                runTimed = workload::FeTimedScope();
                runT = runTimed ? port_host_time_ns() : 0;
            } else if (!st && runOpen && cmd != 0x00 && cmd != 0x44 && cmd != 0x48) {
                runClose();
            }
        }
        if (__builtin_expect(noopMode == 2, 0) && (cmd == 0x61 || cmd == 0x08 || cmd == 0x10)) NoopCheck(cmdStart, end);
        if (cmd == 0x00) {
            if constexpr (kC) ck = sc::kPad;
            // (display lists are padded with zeros to a multiple of 32 bytes: skip a word at a time)
            while (end - p >= 4) {
                uint32_t w;
                std::memcpy(&w, p, 4);
                if (w) break;
                p += 4;
            }
            continue;
        }
        if (cmd == 0x61) {
            if (end - p < 4) { cut = cmdStart; break; }
            const uint32_t v = be32(p);
            p += 4; // (before the write: a display copy ends the frame from inside it)
            if constexpr (kC) { ck = (v >> 24) == 0xFF ? sc::kMark : sc::kBp; sc::TopBp(v); }
            if (bpFast != 0 && BpWriteIsNoop(v)) {
                if (diag::kEnabled && bpFast == 2) BpCheckNoop(v);
                ++stateCmds;
                // (the GX library writes BP registers in bursts: stay in a tight loop while they are no-ops)
                if (noopMode == 1) {
                    p = NoopRun(p, end, stateCmds); // (GXV_NOOP_FILTER: BP, CP and XF repeats in a row)
                } else if (bpFast == 1 && !kC && noopMode == 0) { // (kC: each command counted on its own)
                    // (the mask is the default here, and a no-op write leaves it so: the register's value is the test)
                    while (end - p >= 5 && p[0] == 0x61) {
                        const uint32_t rv = be32(p + 1);
                        if (kBpEffect.t[rv >> 24] || g.bp[rv >> 24] != (rv & 0x00FFFFFFu)) break;
                        p += 5;
                        ++stateCmds;
                    }
                }
                continue;
            }
            if (__builtin_expect(J3nCensus(), 0) && !inList) J3nNoteBp(v);
            if (__builtin_expect(g_j3n2On, 0) && !inList) J3n2NoteBp(v); // (GXV_J3DNATIVE=4 / 5)
            WriteBP(v);
            ++stateCmds;
            // (GXV_STREAM_CENSUS: a BEGIN marker, or the counted picture's display copy, switches the form for the rest of the call)
            if (!inList && __builtin_expect(sc::g_on != kC, 0)) { switchAt = p; break; }
        } else if (cmd == 0x08) {
            if (end - p < 5) { cut = cmdStart; break; }
            const uint8_t reg = p[0];
            const uint32_t v = be32(p + 1);
            p += 5;
            if constexpr (kC) { ck = sc::kCp; sc::TopCp(reg, v); }
            if (noopMode == 1 && CpIsNoop(reg, v)) { // (GXV_NOOP_FILTER)
                ++stateCmds;
                p = NoopRun(p, end, stateCmds);
                continue;
            }
            WriteCPInline(reg, v);
            ++stateCmds;
        } else if (cmd == 0x10) {
            if (end - p < 4) { cut = cmdStart; break; }
            const uint32_t n = static_cast<uint32_t>(be16(p) & 0xF) + 1u;
            const uint32_t addr = be16(p + 2);
            if (static_cast<uint32_t>(end - p) < 4u + n * 4u) { cut = cmdStart; break; }
            p += 4;
            if (noopMode == 1 && XF_IS_NOOP(addr, n, p)) { // (GXV_NOOP_FILTER)
                p += n * 4u;
                ++stateCmds;
                p = NoopRun(p, end, stateCmds);
                continue;
            }
            if (__builtin_expect(J3nCensus(), 0) && !inList) J3nNoteXf(addr, n, p);
            if (__builtin_expect(g_j3n2On, 0) && !inList) J3n2NoteXf(addr, n); // (GXV_J3DNATIVE=4 / 5)
            if constexpr (kC) { ck = sc::kXf; sc::TopXf(addr, n, p); }
            WriteXFBE(addr, n, p);
            p += n * 4u;
            ++stateCmds;
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (end - p < 4) { cut = cmdStart; break; }
            const uint32_t index = be16(p);
            const uint32_t v = be16(p + 2);
            p += 4;
            const uint32_t n = (v >> 12) + 1u, addr = v & 0xFFF;
            const int which = (cmd - 0x20) >> 3;
            SyncArrays(static_cast<uint16_t>(1u << (12 + which)));
            bool le = false;
            const uint8_t* src = ArrayElem(A_POSMTXARRAY + which, index, n * 4u, le);
            if constexpr (kC) { ck = sc::kIdx; sc::TopIdx(which, addr, n, src != kZero ? src : nullptr, le); }
            if (src != kZero) {
                if (le) { // matrices the game's code stored: host words
                    uint32_t w[16];
                    std::memcpy(w, src, n * 4u);
                    WriteXF(addr, n, w);
                } else {
                    WriteXFBE(addr, n, src);
                }
            }
            ++stateCmds;
        } else if (cmd == 0x40) {
            if (end - p < 8) { cut = cmdStart; break; }
            const uint32_t addr = be32(p), size = be32(p + 4);
            p += 8;
            if constexpr (kC) ck = sc::kCall;
            if (inList) {
                VGX_LOG_N(8, "CALL_DL inside a display list ignored\n");
            } else if (addr && size) {
                uint32_t avail = 0;
                const uint8_t* list = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
                if (list && avail >= size) {
                    if (pfDepth) {
                        ahead.Step(cmdStart, p, end, pfDepth);
                        ResolvePrefetch();
                    }
                    workload::FeScope feList(workload::FeList);
                    if (__builtin_expect(J3nCensus(), 0)) J3nNoteList(size);
                    sc::CallSnap snap;
                    if constexpr (kC) sc::CallBefore(list, size, snap);
                    if (__builtin_expect(g_j3n2On, 0)) J3n2Plain(list, size); // (GXV_J3DNATIVE=4 / 5: a token of the chain, or its end)
                    else CallList(list, size);
                    if constexpr (kC) sc::CallAfter(list, size, snap);
                }
                else VGX_LOG_N(16, "CALL_DL %08X + %u: not mapped\n", addr, size);
            }
        } else if (cmd == 0x44 || cmd == 0x48) {
            if constexpr (kC) ck = sc::kNop;
            continue; // metrics / invalidate vertex cache: one byte, nothing to do
        } else if (cmd == kRetainedOp && !inList) {
            // (GXV_RETAINED: a J3D shape's handle, then its CALL_DL; decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_retained. Only
            // ever in the top-level stream. The pair is taken as one command: a handle without its call is left for the next pass.)
            if (end - p < 13) { cut = cmdStart; break; }
            if constexpr (kC) ck = sc::kShape;
            if (p[4] != 0x40) { p += 4; ++stateCmds; continue; } // (a handle without a call after it: nothing)
            const uint32_t handle = be32(p), addr = be32(p + 5), size = be32(p + 9);
            p += 13;
            if (addr && size) {
                uint32_t avail = 0;
                const uint8_t* list = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
                if (list && avail >= size) {
                    if (pfDepth) {
                        ahead.Step(cmdStart + 5, p, end, pfDepth);
                        ResolvePrefetch();
                    }
                    workload::FeScope feList(workload::FeList);
                    if (__builtin_expect(J3nCensus(), 0)) J3nNoteShape();
                    sc::CallSnap snap;
                    if constexpr (kC) sc::CallBefore(list, size, snap);
                    g_tlSite = 1; // (GXV_TINYLIST_CENSUS: the call site)
                    if (__builtin_expect(g_j3n2On, 0)) J3n2Shape(list, size, handle); // (GXV_J3DNATIVE=4 / 5)
                    else if (RetainedOn()) CallListRetained(list, size, handle);
                    else CallList(list, size);
                    g_tlSite = 0;
                    if constexpr (kC) { sc::CallAfter(list, size, snap); sc::Shape(); }
                }
                else VGX_LOG_N(16, "CALL_DL %08X + %u: not mapped\n", addr, size);
            }
        } else if (cmd == kFxMergeOp && !inList) {
            // (GXV_FXMERGE: a particle command of the game's GX library, FxMergeRun above; only ever in the top-level stream)
            if (end - p < 2) { cut = cmdStart; break; }
            const uint32_t n = be16(p);
            if (static_cast<uint32_t>(end - p) < 2u + n) { cut = cmdStart; break; }
            if (__builtin_expect(J3nCensus(), 0)) J3nNoteFx();
            if constexpr (kC) ck = sc::kFx;
            FxMergeRun(p + 2, n);
            p += 2u + n;
            ++stateCmds;
        } else if (cmd & 0x80) {
            if (end - p < 2) { cut = cmdStart; break; }
            const uint32_t count = be16(p);
            const Plan& plan = GetPlan(cmd & 7);
            const uint32_t vsize = plan.streamSize;
            const uint32_t avail = static_cast<uint32_t>(end - p) - 2u;
            const uint64_t used = static_cast<uint64_t>(count) * vsize;
            if (used > avail) {
                if (!inList) { cut = cmdStart; break; }
                VGX_LOG_N(16, "draw in a display list runs past its end: %u verts x %u bytes > %u\n", count, vsize, avail);
            }
            p += 2;
            if constexpr (kC) { ck = sc::kDraw; cverts = count; }
#if defined(VGX_CLONE_B)
            two::g_fmtSeen |= 1u << (cmd & 7); // (GXV_FE_TWOCORE: the formats a list's draws use, for the scan's skip of draw-only lists)
#endif
            if (vsize != 0 && count != 0) {
                if (__builtin_expect(J3nCensus(), 0) && !inList) J3nNoteDraw();
                ++g_sd.draws[inList]; g_sd.verts[inList] += count;
                if (__builtin_expect(g_fpRec != nullptr, 0) && inList) FpRecBefore(cmd, p, count, avail); // (GXV_FEPLAN=2)
                DrawStreamPlan(cmd & 0xF8, cmd & 7, plan, p, count, avail, inList ? 1 : 0);
                if (__builtin_expect(g_fpRec != nullptr, 0) && inList) FpRecAfter();
                ++stats::Cur().primsDL;
                stats::Cur().vertsDL += count;
            }
            if (used > avail) break;
            p += used;
        } else if ((cmd == kJ3nOp || cmd == kJ3nEndOp) && !inList) {
            // (GXV_J3DNATIVE: a bracket, decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_bracketed: id, versions, ext, flags, the call, the end
            // marker. Only ever in the top-level stream. Taken as one command; a stray end marker, or a begin without its call, is consumed.)
            if constexpr (kC) ck = sc::kJ3n;
            if (cmd == kJ3nEndOp) continue;
            if (end - p < 21) { cut = cmdStart; break; }
            if (p[20] != 0x40) { p += 20; ++stateCmds; continue; }           // (a begin without a call after it: nothing)
            if (end - p < 30) { cut = cmdStart; break; }                      // (the call and the end marker are still to come)
            if (p[29] != kJ3nEndOp) { p += 20; ++stateCmds; continue; }       // (no end marker: the call is a plain one)
            const uint32_t id = be32(p), vA = be32(p + 4), vB = be32(p + 8), ext = be32(p + 12), fl = be32(p + 16);
            const uint32_t addr = be32(p + 21), size = be32(p + 25);
            p += 30;
            if (addr && size) {
                uint32_t avail = 0;
                const uint8_t* list = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
                if (list && avail >= size) {
                    if (pfDepth) {
                        ahead.Step(cmdStart + 21, p, end, pfDepth);
                        ResolvePrefetch();
                    }
                    workload::FeScope feList(workload::FeList);
                    g_tlSite = 3; // (GXV_TINYLIST_CENSUS: the call site)
                    J3nCall(list, size, id, vA, vB, ext, fl);
                    g_tlSite = 0;
                }
                else VGX_LOG_N(16, "CALL_DL %08X + %u: not mapped\n", addr, size);
            }
        } else {
            VGX_LOG_N(16, "unknown GX opcode 0x%02X in a %s (%u bytes left)\n", cmd, inList ? "display list" : "command stream", static_cast<unsigned>(end - p));
            if (inList) break; // (the top-level stream resynchronises a byte at a time, like the console would not)
        }
    }
    if (runOpen) runClose();
    if (!cont) { ++g_sd.calls[inList]; g_sd.bytes[inList] += nbytes; }
    g_sd.stateCmds[inList] += stateCmds;
    if constexpr (kC) {
        // (the last command ends where the parse stopped: a cut command is not consumed and not counted)
        const uint8_t* const stop = switchAt ? switchAt : (cut ? cut : (p < end ? p : end));
        if (cstart && stop > cstart) sc::Cmd(ck, static_cast<uint32_t>(stop - cstart), cverts);
        sc::Parsed(static_cast<uint32_t>(stop - start));
        sc::ParseEnd();
    }
    if (switchAt) return static_cast<uint32_t>(switchAt - start) + ProcessStreamT<!kC>(switchAt, static_cast<uint32_t>(end - switchAt), inList, true);
    if (jumped) return static_cast<uint32_t>(jumped - start); // (may be past `end`: the caller goes on from there)
    return static_cast<uint32_t>((cut ? cut : (p < end ? p : end)) - start);
}
void NoopFilterCounters(uint64_t out[2]) { out[0] = g_noopChecked; out[1] = g_noopBad; } // (GXV_NOOP_FILTER=2: commands checked, mismatching)
// verifyfix31: the verify modes' counts for the game log (port_vgx_verify_summary, every 300 pictures from port_j3d_picture_begin). stderr is not captured by the
// console build, and the retained verify's count was only in the log with GXV_WORKLOAD_PROBE=1. Cumulative counts; the line is made only while a verify is on.
// (The counters are written by the front end's worker and read here without a lock: a log line, a torn read at worst shows an older count.)
bool VerifySummaryLine(char* out, size_t n) {
    int at = 0;
    out[0] = '\0';
    const int nf = NoopMode();
    const char* const want = getenv("GXV_NOOP_FILTER");
    if (nf == 2 || (want != nullptr && want[0] == '2')) {
        uint64_t c[2];
        NoopFilterCounters(c);
        at += std::snprintf(out + at, n - at, "noopfilter: GXV_NOOP_FILTER requested %s, effective mode %d%s, %llu checked, %llu mismatching; ", want ? want : "0", nf,
                            nf == 2 ? "" : " (verify not active here: the quiet build / clone / two-core player run it as 1 or 0)", static_cast<unsigned long long>(c[0]),
                            static_cast<unsigned long long>(c[1]));
    }
    const char* const rt = getenv("GXV_RT_INST");
    const char* const rr = getenv("GXV_RETAINED");
    if ((rt != nullptr && rt[0] == '2') || (rr != nullptr && rr[0] == '2')) {
        uint64_t r[7];
        RetainedCounters(r);
        at += std::snprintf(out + at, n - at, "retained verify (GXV_RT_INST=2 / GXV_RETAINED=2): %llu checked, %llu mismatching; ", static_cast<unsigned long long>(r[5]), static_cast<unsigned long long>(r[6]));
    }
    return at > 0;
}
void StateFastCounters(uint64_t out[4]) { out[0] = g_sfChecked; out[1] = g_sfBad; out[2] = g_sfCmds; out[3] = g_sfRuns; } // (GXV_FE_STATEFAST=2: commands checked, mismatching; =1/2: commands taken, runs)
uint32_t ProcessStream(const uint8_t* p, uint32_t nbytes, bool inList) {
    // (GXV_STREAM_CENSUS: a counted picture's top-level stream; one flag test otherwise)
    if (__builtin_expect(sc::g_on, 0) && !inList) return ProcessStreamT<true>(p, nbytes, inList, false);
    return ProcessStreamT<false>(p, nbytes, inList, false);
}


// ------------------------------------------------------- state-only display lists, pre-parsed
// The geometry cache (vgx_dlcache.cpp) keeps lists that draw. The game's material display lists (J3D: TEV stages, colours,
// texture bindings, blend and z modes, matrix loads) hold only state commands, are called again every picture with the same
// bytes, and were parsed again each time: a big-endian opcode walk, a byte swap per word and a branch per command, ProcessStream's
// largest share. Such a list is parsed once into host-order operations and replayed while the write generation of its bytes (or,
// after a write nearby, its content hash) is unchanged. A replay makes the same calls in the same order as ProcessStream would
// (WriteBP with the same no-op test, WriteCP, WriteXF / WriteXFWord, the indexed matrix loads resolved at replay time against the
// arrays bound then), so the state that results is the same. A list with a draw, a call or an unknown opcode is not cached (what
// follows a draw depends on the vertex format at that moment). A list that changes on most calls is left alone for a while.
//   GXV_STATELIST=0  off;  =2  verify: a state-only list's replay is compared with ProcessStream on a copy of the state (logged)
namespace {
struct SlOp {
    uint8_t kind;    // 1 BP with an effect on every write, 2 CP, 3 XF (direct words), 4 XF indexed load,
                     // 5 a run of other BP writes (b of them, from words[v]),
                     // 6 a run of TEV colour / konst writes (BP 0xE0..0xE7; b of them, from words[v]; GXV_REGSHADOW),
                     // 7 one XF register word other than the matrix indices (b: address, v: the word; GXV_REGSHADOW)
    uint8_t a;       // CP register / XF word count / indexed: which array (0..3)
    uint16_t b;      // XF address / indexed: element index
    uint32_t v;      // BP register and value / CP value / XF: first word in words / indexed: (n << 16) | addr
};
struct MixPlan; // (GXV_FEPLAN, feplan31: a mixed list's plan, below SlRun)
struct MixPlanDel { void operator()(MixPlan* m) const; };
// (the fields a fast hit reads, StateListFastHit and SlRun's delta path, are the entry's first 32 bytes: one cache line of the console,
// the entry aligned to it. A material hit read three to five lines of the entry before (the fields were spread over its ~110 bytes,
// the delta's address in the last of them) and wrote two of them back (`changes`, `lastFrame`) on every hit; now it reads one and
// writes none. lastFrame was only ever compared with 0: `live` is that comparison, set where lastFrame was set, to frame != 0.)
struct alignas(32) SlEntry {
    uint32_t nbytes = 0;
    bool usable = false;     // false: a list with draws, calls or opcodes this does not handle
    uint8_t delta0 = 0;      // (GXV_SLDELTA) 0 not compiled; 2 `delta` holds the list as register deltas; 3 not compilable;
                             // 4 as 2, in the compact layout (GXV_FECUT_SLHOT, see SlCompileDelta)
    bool live = false;       // (was lastFrame != 0) parsed (or replayed) and not since found changed or raced
    uint8_t changes = 0;     // consecutive calls that found the bytes changed (0..3)
    const uint32_t* dp = nullptr; // delta.data() while delta0 is 2 or 4
    uint32_t dbytes = 0;          // (its size in bytes, for GXV_FECUT_HINT; in the alignment gap before `gen` on the console)
    uint64_t gen = 0, quietUntil = 0;
    // ---- the rest
    uint64_t hash = 0;
    std::vector<uint8_t> bytes; // (GXV_SLBYTES: the list as parsed, to tell a rewrite with the same bytes from a change)
    uint64_t parseFrame = 0; // (GXV_SLDELTA) the frame of the last parse
    uint8_t quietWhy = 0, quietCmd = 0; // (GXV_FE_CENSUS, fedl25: why quietUntil was set: 3 refused (the opcode), 4 churn)
    std::vector<SlOp> ops;
    std::vector<uint32_t> words;
    std::vector<uint32_t> delta;
    std::unique_ptr<MixPlan, MixPlanDel> mix; // (GXV_FEPLAN: usable false and the list holds draws / calls: its exact plan, or none)
    uint8_t mixMiss = 0;                       // (GXV_FEPLAN: consecutive replays that found a draw's vertex size changed)
};
// Entries by list address: open addressing over a power-of-two table (an unordered_map's prime buckets cost a software
// division per lookup on the Vita), cleared as a whole when it fills.
constexpr uint32_t kSlSlots = 32768, kSlMax = 16384;
struct SlSlot { const uint8_t* list; SlEntry* e; };
SlSlot g_slTable[kSlSlots];
std::vector<std::unique_ptr<SlEntry>> g_slStore;
inline bool SameBytes(const uint8_t* a, const uint8_t* b, uint32_t n) {
    if (!SameWords(a, b, n >> 2)) return false;
    for (uint32_t i = n & ~3u; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
uint32_t SlHash(const uint8_t* p) { return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) >> 5) * 2654435761u; }
SlEntry* SlFind(const uint8_t* list) {
    uint32_t i = SlHash(list) & (kSlSlots - 1);
    for (;;) {
        SlSlot& s = g_slTable[i];
        if (s.list == list) return s.e;
        if (!s.list) {
            g_slStore.push_back(std::make_unique<SlEntry>());
            s.list = list;
            s.e = g_slStore.back().get();
            return s.e;
        }
        i = (i + 1) & (kSlSlots - 1);
    }
}
// The list last looked up per address class, in front of the table. GXV_SLFAST (read once, default 1) decides how many classes
// are used: 1,024, or (0) 256 as before. A picture of the Comet Observatory calls some 500 material lists: in 256 classes only
// about one in eight had its class to itself from one picture to the next, so StateListFastHit missed for the rest, and they
// went the geometry cache's long way round (array sync, guest-RAM test, bypass map, then StateListCall): "list-other", ~430 a picture.
constexpr uint32_t kSlFrontMax = 1024;
struct SlFront { const uint8_t* list = nullptr; SlEntry* e = nullptr; } g_slFront[kSlFrontMax];
inline int SlFastMode() {
    const int m = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_SLFAST", 1))));
    return m;
}
inline SlFront& SlFrontOf(const uint8_t* list) {
    const uint32_t mask = static_cast<uint32_t>(VGX_KNOB(SlFastMode() != 0 ? kSlFrontMax - 1u : 255u));
    return g_slFront[(reinterpret_cast<uintptr_t>(list) >> 5) & mask];
}
// The entry of a list, if it has one (SlFind without the insertion: the table is at most half full, so a probe ends).
SlEntry* SlLookup(const uint8_t* list) {
    uint32_t i = SlHash(list) & (kSlSlots - 1);
    for (;;) {
        const SlSlot& s = g_slTable[i];
        if (s.list == list) return s.e;
        if (!s.list) return nullptr;
        i = (i + 1) & (kSlSlots - 1);
    }
}
inline bool SlKeepBytes() {
    const bool keep = VGX_KNOB(EnvInt("GXV_SLBYTES", 1) != 0) != 0;
    return keep;
}
uint64_t g_slHits = 0, g_slBuilds = 0, g_slRefused = 0, g_slChecked = 0, g_slBad = 0;
uint64_t g_slClears = 0; // (heap23b) the store's clears at kSlMax entries
uint32_t g_slEpoch = 1;  // (j3n1) moves whenever entries are freed (the drop at a scene's destruction, the clear): GXV_J3DNATIVE's records point at entries
uint64_t g_slFastFront = 0, g_slFastTable = 0, g_slFastSame = 0; // (GXV_SLFAST: StateListFastHit's hits by the front, by the table; rewrites with the same bytes)

uint8_t g_slWhy = 0, g_slBadCmd = 0; // (GXV_FE_CENSUS, fedl25: StateListWhyLast)
// One state command of a list (its opcode `cmd` already read, p just after it), appended to e's operations: 1 taken (p moved past
// it), 0 cut by the end of the list (nothing taken: ProcessStream stops there too), -1 not a state command (p unchanged).
// (feplan31: SlParse's loop body, shared with the mixed lists' plans, GXV_FEPLAN)
inline int SlParseCmd(SlEntry& e, uint8_t cmd, const uint8_t*& p, const uint8_t* const end, bool shadow) {
    if (cmd == 0x61) {
        if (end - p < 4) return 0;
        const uint32_t rv = be32(p);
        p += 4;
        if (kBpEffect.t[rv >> 24]) {
            e.ops.push_back(SlOp{1, 0, 0, rv});
        } else {
            const uint8_t kind = (shadow && (rv >> 24) >= 0xE0 && (rv >> 24) <= 0xE7) ? 6 : 5;
            if (e.ops.empty() || e.ops.back().kind != kind || e.ops.back().b == 0xFFFF)
                e.ops.push_back(SlOp{kind, 0, 0, static_cast<uint32_t>(e.words.size())});
            ++e.ops.back().b;
            e.words.push_back(rv);
        }
        return 1;
    }
    if (cmd == 0x08) {
        if (end - p < 5) return 0;
        e.ops.push_back(SlOp{2, p[0], 0, be32(p + 1)});
        p += 5;
        return 1;
    }
    if (cmd == 0x10) {
        if (end - p < 4) return 0;
        const uint32_t n = static_cast<uint32_t>(be16(p) & 0xF) + 1u;
        const uint32_t addr = be16(p + 2);
        if (static_cast<uint32_t>(end - p) < 4u + n * 4u) return 0;
        p += 4;
        if (shadow && n == 1 && addr >= 0x1000 && addr < 0x1058 && addr != 0x1018 && addr != 0x1019) {
            e.ops.push_back(SlOp{7, 1, static_cast<uint16_t>(addr), be32(p)});
            p += 4;
            return 1;
        }
        e.ops.push_back(SlOp{3, static_cast<uint8_t>(n), static_cast<uint16_t>(addr), static_cast<uint32_t>(e.words.size())});
        for (uint32_t i = 0; i < n; ++i) e.words.push_back(be32(p + i * 4));
        p += n * 4u;
        return 1;
    }
    if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
        if (end - p < 4) return 0;
        const uint32_t index = be16(p), v = be16(p + 2);
        p += 4;
        e.ops.push_back(SlOp{4, static_cast<uint8_t>((cmd - 0x20) >> 3), static_cast<uint16_t>(index), (((v >> 12) + 1u) << 16) | (v & 0xFFF)});
        return 1;
    }
    return -1;
}
// Parse; false if the list holds something the replay does not reproduce.
bool SlParse(SlEntry& e, const uint8_t* p, uint32_t nbytes) {
    const bool shadow = RegShadow() != 0;
    e.ops.clear();
    e.words.clear();
    const uint8_t* const end = p + nbytes;
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) {
            while (end - p >= 4) {
                uint32_t w;
                std::memcpy(&w, p, 4);
                if (w) break;
                p += 4;
            }
            continue;
        }
        if (cmd == 0x44 || cmd == 0x48) continue;
        const int r = SlParseCmd(e, cmd, p, end, shadow);
        if (r > 0) continue;
        if (r == 0) return true;  // (a cut command: ProcessStream stops there too)
        g_slBadCmd = cmd;
        return false;  // a draw, a call or an unknown opcode
    }
    return true;
}

void SlReplay(const SlEntry& e, int bpFast) {
    ++stats::Cur().streams;
    stats::Cur().streamBytes += e.nbytes;
    for (const SlOp& op : e.ops) {
        switch (op.kind) {
        case 1:
            if (bpFast != 0) {
                const uint32_t reg = op.v >> 24;
                if (!kBpEffect.t[reg] && g.bpMask == 0x00FFFFFFu && g.bp[reg] == (op.v & 0x00FFFFFFu)) break;
            }
            WriteBP(op.v);
            break;
        case 5: {
            // Plain registers: writing the value a register holds changes nothing. Any write here leaves the next-write
            // mask at its default (only BP 0xFE sets it, and that one is a kind 1 op), so only the first can see a mask.
            const uint32_t* r = &e.words[op.v];
            const uint32_t* const rend = r + op.b;
            if (bpFast == 0 || g.bpMask != 0x00FFFFFFu) WriteBP(*r++);
            if (bpFast == 0) {
                for (; r < rend; ++r) WriteBP(*r);
            } else {
                for (; r < rend; ++r) {
                    const uint32_t rv = *r, reg = rv >> 24, nv = rv & 0x00FFFFFFu;
                    const uint32_t ov = g.bp[reg];
                    if (ov == nv) continue;
                    if (kBpPlain.t[reg]) { // (WriteBP's tail, inline: the mask is the default here)
                        g.bp[reg] = nv;
                        MarkBP(static_cast<uint8_t>(reg), ov, nv);
                    } else if (reg >= 0x80 && reg <= 0xBB) { // (WriteBP's texture-unit path, inline)
                        g.bp[reg] = nv;
                        LodRegChanged(reg, ov, nv);
                        if (const uint8_t bit = kBpPlain.tex[reg]) {
                            g.texDirty = true;
                            g.texSlotDirty |= bit;
                            g.bpTexStale |= bit;
                        }
                    } else {
                        WriteBP(rv);
                    }
                }
            }
            break;
        }
        case 6: {
            // TEV colour halves: a write of the colour the half holds changes only bp[] (see State::tevRaw).
            const uint32_t* r = &e.words[op.v];
            const uint32_t* const rend = r + op.b;
            if (bpFast == 0 || g.bpMask != 0x00FFFFFFu) WriteBP(*r++);
            if (bpFast == 0 || RegShadow() == 2) {
                for (; r < rend; ++r) WriteBP(*r);
            } else {
                for (; r < rend; ++r) {
                    const uint32_t rv = *r, reg = rv >> 24, nv = rv & 0x00FFFFFFu;
                    if (g.tevRaw[TevRawSlot(reg, nv)] != nv) WriteTevColor(static_cast<uint8_t>(reg), nv);
                    else g.bp[reg] = nv;
                }
            }
            break;
        }
        case 7: {
            // (WriteXFWord's register path)
            uint32_t& d = g.xfReg[op.b - 0x1000];
            if (d != op.v) { const uint32_t o = d; d = op.v; MarkXFReg(op.b - 0x1000u, o, op.v); }
            break;
        }
        case 2: WriteCPInline(op.a, op.v); break;
        case 3:
            if (op.a > 1) WriteXF(op.b, op.a, &e.words[op.v]);
            else WriteXFWord(op.b, e.words[op.v]);
            break;
        case 4: {
            const uint32_t n = op.v >> 16, addr = op.v & 0xFFF;
            SyncArrays(static_cast<uint16_t>(1u << (12 + op.a)));
            bool le = false;
            const uint8_t* src = ArrayElem(A_POSMTXARRAY + op.a, op.b, n * 4u, le);
            if (src != kZero) {
                if (le) {
                    uint32_t w[16];
                    std::memcpy(w, src, n * 4u);
                    WriteXF(addr, n, w);
                } else {
                    WriteXFBE(addr, n, src);
                }
            }
            break;
        }
        }
    }
}

// ------------------------------------------------------- register deltas (GXV_SLDELTA)
// A J3D material list writes ~80 BP registers and ~10 XF words: the TEV colours' BG halves three times (the hardware's write delay),
// the texture-coordinate scales and the indirect registers twice or three times behind a BP 0xFE mask, every register once with the
// value it holds after the list. SlReplay walks that sequence op by op (a dispatch, a mask write and a full WriteBP for each masked
// one, the repeats). Most lists are "single-valued": every write of a register (BP: with the same mask) writes the same value, so
// the first write of each register is the only one that can change anything, and its effect does not depend on where the others
// are. Such a list is compiled, once, into the distinct writes grouped by kind, each a compare with the register and, when it
// differs, the store and the marks SlReplay's path for that write makes:
//   - BP plain registers (WriteBP's tail): store, MarkBP's classes collected and the key hash XOR collected, its KeyZ(register,
//     new value) computed at the compile; the flags and the hash are applied once at the end (ORs and XORs: order-free);
//   - BP plain registers behind a mask: (old & ~mask) | (value & mask), the same marks;
//   - texture-unit registers: WriteBP's texture path (in list order: LodRegChanged reads the unit's other register);
//   - TEV colour / konst halves: kind 6's test and WriteTevColor (in list order: TevOorUpdate), then bp[] of each address set to
//     its last write (the halves share the address; nothing reads bp[0xE0..0xE7] in between);
//   - single XF register words (kind 7's path; a multi-word register write is that path word by word, WriteXF);
//   - other XF writes: WriteXF / WriteXFWord as SlReplay calls them, whole, in list order (the matrix memories mark rows by write);
//   - CP writes: WriteCPInline as SlReplay calls it, in list order.
// The kinds touch disjoint state (the matrix index registers, which CP 0x30 / 0x40 and XF 0x1018 / 0x1019 share, are refused when
// both are written), and within a kind the order is kept, so the registers, the flags and the key hash after the delta are exactly
// SlReplay's: a repeated write of the value a register holds changes nothing there either. Not compiled (SlReplay as before): a
// register written with two values or with and without a mask, a mask before a TEV / texture register or at the end, indexed
// matrix loads, BP registers with side effects (tokens, copies, palette loads), CP registers outside the arrays, matrix indices and
// vertex formats. Used only where SlReplay's fast paths are (GXV_BPFAST=1, GXV_REGSHADOW=1) and the next-write mask is the default.
//   GXV_SLDELTA=1 (default) on; 0 off (SlReplay); 2 (diagnostics builds) both from the same state, the State compared (logged).
enum : uint32_t { kDBpKey, kDBp, kDBpMask, kDTex, kDTev, kDTevBp, kDXfWKey, kDXfW, kDXfB, kDCp, kDSections };
// Compile e.ops / e.words into e.delta (layout: kDSections counts, then the sections in that order; records, rv being a BP command
// word, register << 24 | value: BpKey 3 words {rv, KeyZ(register, value) high, low}, Bp 1 {rv}, BpMask 2 {register << 24 | value &
// mask, ~mask}, Tex 1 {rv}, Tev 1 {tevRaw slot << 24 | value}, TevBp 1 {rv}, XfWKey 4 {register, value, KeyZ(0x100 + register, value) high, low}, XfW 2 {register, value}, XfB 1 + n {address | n << 16, words}, Cp 2 {reg, value}).
// KeyZ of the value each BP register was last given here (GXV_SLDELTA): the key hash's "out" term of a changed register is
// usually the value an earlier delta put there. Checked by the value, so writes elsewhere only make it miss.
struct KzCache { uint32_t v, a, b; };
KzCache g_kz[0x159]; // (KeyZ's index: BP registers 0..255, XF registers 0x100 + r)
inline uint64_t KeyZCached(uint32_t reg, uint32_t v) {
    const KzCache& c = g_kz[reg];
    if (c.v == v) return (static_cast<uint64_t>(c.a) << 32) | c.b;
    return KeyZ(reg, v);
}
inline void KeyZRemember(uint32_t reg, uint32_t v, uint64_t z) { g_kz[reg] = KzCache{v, static_cast<uint32_t>(z >> 32), static_cast<uint32_t>(z)}; }
// GXV_FECUT_SLHOT (read once; default 0 = the layout above): the compact layout. A delta replay compares every record with the
// register and, mostly, finds it unchanged; the precomputed KeyZ words of the key-register records (BpKey 2 of its 3 words, XfWKey 2
// of its 4) are read only for the records that changed, but they sit between the others and take a third of a J3D material list's
// delta (~165 words, ~660 bytes, of which ~53 words KeyZ; tests/vgx_fe_work.cpp's lists taken from the game). The compact layout keeps
// BpKey as 1 word {rv} and XfWKey as 2 {register, value}, and SlDeltaApply works KeyZ(register, new value) out for a changed one (the
// same pure function the compile used: the same key hash). A material hit then reads about a third fewer lines of its delta, which
// the game's turn has pushed out of the caches since the last picture, for ~20 instructions per changed key register.
inline bool SlHotCompact() { return VGX_KNOB(EnvInt("GXV_FECUT_SLHOT", 0) != 0) != 0; }
bool SlCompileDelta(SlEntry& e) {
    static const bool kzInit = [] { for (uint32_t r = 0; r < 0x159; ++r) KeyZRemember(r, 0, KeyZ(r, 0)); return true; }();
    (void)kzInit;
    e.delta.clear();
    // (the player's thread only: scratch kept between compiles, no allocation in the steady state)
    static std::vector<uint32_t> sec[kDSections];
    static std::vector<uint32_t> xfSeen; // XF address, word pairs (a list writes a few dozen)
    for (std::vector<uint32_t>& v : sec) v.clear();
    xfSeen.clear();
    uint32_t bpSeen[8] = {}, bpVal[256], bpMaskOf[256]; // (register seen, its value and mask)
    uint32_t cpSeen[8] = {}, cpVal[256];
    uint32_t tevSeen = 0, tevVal[16];
    int tevBpAt[8];
    for (int& i : tevBpAt) i = -1;
    bool cpMtxIdx = false, xfMtxIdx = false;
    uint32_t pendMask = 0;
    bool pend = false;
    auto xfWord = [&](uint32_t addr, uint32_t w, bool& dup) {
        for (size_t i = 0; i < xfSeen.size(); i += 2)
            if (xfSeen[i] == addr) { dup = true; return xfSeen[i + 1] == w; }
        dup = false;
        xfSeen.push_back(addr);
        xfSeen.push_back(w);
        return true;
    };
    const bool compact = SlHotCompact();
    auto xfReg = [&](uint32_t r, uint32_t v) { // (an XF register word: with KeyZ(0x100 + r, v) when the shader keys read it)
        if (IsKeyXF(r)) {
            const uint64_t z = KeyZ(0x100u + r, v);
            sec[kDXfWKey].push_back(r);
            sec[kDXfWKey].push_back(v);
            if (compact) return; // (GXV_FECUT_SLHOT: worked out at the replay)
            sec[kDXfWKey].push_back(static_cast<uint32_t>(z >> 32));
            sec[kDXfWKey].push_back(static_cast<uint32_t>(z));
        } else {
            sec[kDXfW].push_back(r);
            sec[kDXfW].push_back(v);
        }
    };
    auto bpWrite = [&](uint32_t rv) {
        const uint32_t reg = rv >> 24, val = rv & 0x00FFFFFFu;
        const uint32_t mask = pend ? pendMask : 0x00FFFFFFu;
        pend = false;
        if (bpSeen[reg >> 5] & (1u << (reg & 31))) return bpMaskOf[reg] == mask && bpVal[reg] == (val & mask); // (a repeat: changes nothing after the first)
        bpSeen[reg >> 5] |= 1u << (reg & 31);
        bpMaskOf[reg] = mask;
        bpVal[reg] = val & mask;
        if (reg >= 0xE0 && reg <= 0xE7) return false; // (the halves: taken before this)
        if (reg >= 0x80 && reg <= 0xBB) {
            if (mask != 0x00FFFFFFu) return false;
            sec[kDTex].push_back(rv);
            return true;
        }
        if (!kBpPlain.t[reg]) return false;
        if (mask != 0x00FFFFFFu) {
            sec[kDBpMask].push_back((reg << 24) | (val & mask));
            sec[kDBpMask].push_back(~mask);
        } else if (kBpMark.t[reg] & kMarkKey) {
            const uint64_t z = KeyZ(reg, val);
            sec[kDBpKey].push_back(rv);
            if (compact) return true; // (GXV_FECUT_SLHOT: worked out at the replay)
            sec[kDBpKey].push_back(static_cast<uint32_t>(z >> 32));
            sec[kDBpKey].push_back(static_cast<uint32_t>(z));
        } else {
            sec[kDBp].push_back(rv);
        }
        return true;
    };
    for (const SlOp& op : e.ops) {
        switch (op.kind) {
        case 1:
            if ((op.v >> 24) != 0xFE || pend) return false;
            pend = true;
            pendMask = op.v & 0x00FFFFFFu;
            break;
        case 5:
        case 6:
            for (uint32_t i = 0; i < op.b; ++i) {
                const uint32_t rv = e.words[op.v + i], reg = rv >> 24;
                if (reg >= 0xE0 && reg <= 0xE7) {
                    if (pend) return false;
                    // (each half of the address: single-valued; bp[] of the address: its last write)
                    const uint32_t nv = rv & 0x00FFFFFFu, slot = TevRawSlot(reg, nv);
                    if (tevSeen & (1u << slot)) {
                        if (tevVal[slot] != nv) return false;
                    } else {
                        tevSeen |= 1u << slot;
                        tevVal[slot] = nv;
                        sec[kDTev].push_back((slot << 24) | nv);
                    }
                    if (tevBpAt[reg - 0xE0] < 0) {
                        tevBpAt[reg - 0xE0] = static_cast<int>(sec[kDTevBp].size());
                        sec[kDTevBp].push_back(rv);
                    } else {
                        sec[kDTevBp][tevBpAt[reg - 0xE0]] = rv;
                    }
                    continue;
                }
                if (!bpWrite(rv)) return false;
            }
            break;
        case 7: {
            bool dup = false;
            if (!xfWord(op.b, op.v, dup)) return false;
            if (!dup) xfReg(op.b - 0x1000u, op.v);
            break;
        }
        case 3: {
            const uint32_t n = op.a, addr = op.b;
            const uint32_t* w = &e.words[op.v];
            const bool regs = addr >= 0x1000 && addr + n <= 0x1058 && !(addr <= 0x1019 && addr + n > 0x1018);
            if (regs) { // (WriteXF's register path, or WriteXFWord's for one word: kind 7's, word by word)
                for (uint32_t i = 0; i < n; ++i) {
                    bool dup = false;
                    if (!xfWord(addr + i, w[i], dup)) return false;
                    if (!dup) xfReg(addr + i - 0x1000u, w[i]);
                }
                break;
            }
            for (uint32_t i = 0; i < n; ++i) {
                bool dup = false;
                if (!xfWord(addr + i, w[i], dup)) return false;
                if (addr + i == 0x1018 || addr + i == 0x1019) xfMtxIdx = true;
            }
            sec[kDXfB].push_back(addr | (n << 16));
            for (uint32_t i = 0; i < n; ++i) sec[kDXfB].push_back(w[i]);
            break;
        }
        case 2: {
            const uint32_t reg = op.a;
            const bool known = (reg >= 0xA0 && reg <= 0xBF) || reg == 0x30 || reg == 0x40 || CpRawSlot(static_cast<uint8_t>(reg)) >= 0;
            if (!known) return false;
            if (reg == 0x30 || reg == 0x40) cpMtxIdx = true;
            if (cpSeen[reg >> 5] & (1u << (reg & 31))) {
                if (cpVal[reg] != op.v) return false;
                break; // (the same value again: WriteCPInline's tests find it there)
            }
            cpSeen[reg >> 5] |= 1u << (reg & 31);
            cpVal[reg] = op.v;
            sec[kDCp].push_back(reg);
            sec[kDCp].push_back(op.v);
            break;
        }
        default:
            return false; // (4: an indexed load reads memory at the replay)
        }
    }
    if (pend || (cpMtxIdx && xfMtxIdx)) return false;
    static constexpr uint32_t kRec[kDSections] = {3, 1, 2, 1, 1, 1, 4, 2, 0, 2};
    static constexpr uint32_t kRecCompact[kDSections] = {1, 1, 2, 1, 1, 1, 2, 2, 0, 2};
    size_t total = kDSections;
    for (const std::vector<uint32_t>& v : sec) total += v.size();
    e.delta.reserve(total);
    for (int s = 0; s < kDSections; ++s) {
        uint32_t n = 0;
        if (s == kDXfB) {
            for (size_t i = 0; i < sec[s].size(); i += 1 + (sec[s][i] >> 16)) ++n;
        } else {
            n = static_cast<uint32_t>(sec[s].size() / (compact ? kRecCompact[s] : kRec[s]));
        }
        e.delta.push_back(n);
    }
    for (int s = 0; s < kDSections; ++s) e.delta.insert(e.delta.end(), sec[s].begin(), sec[s].end());
    e.dp = e.delta.data();
    e.dbytes = static_cast<uint32_t>(e.delta.size() * 4);
    return true;
}

uint64_t g_slCompactApplied = 0; // (GXV_FECUT_SLHOT: delta replays in the compact layout)
// kPart (GXV_J3DNATIVE stage 2, vgx_j3native2.inc): 0 the whole delta (every other caller); 1 only the register part (BP, TEV, XF
// registers: the sections before XfB, with the flags and the key hash they collect); 2 only the CP and XF-memory part (XfB, Cp). The
// sections touch disjoint state (see above), so 1 then 2 (or 2 then 1) leaves exactly what 0 leaves.
template <bool kCompact, int kPart = 0> void SlDeltaApplyT(const SlEntry& e) {
    if (kPart != 2) {
        ++stats::Cur().streams;
        stats::Cur().streamBytes += e.nbytes;
    }
    const uint32_t* p = e.dp; // (delta.data(), from the entry's first line)
    const uint32_t* q;
    const uint32_t* const n = p;
    p += kDSections;
    uint32_t cls = 0;
    uint64_t kh = 0;
    constexpr uint32_t kBpKeyRec = kCompact ? 1 : 3, kXfKeyRec = kCompact ? 2 : 4;
    if (kPart == 2) {
        p += n[kDBpKey] * kBpKeyRec + n[kDBp] + n[kDBpMask] * 2 + n[kDTex] + n[kDTev] + n[kDTevBp] + n[kDXfWKey] * kXfKeyRec + n[kDXfW] * 2;
        goto eager;
    }
    for (q = p + n[kDBpKey] * kBpKeyRec; p < q; p += kBpKeyRec) {
        const uint32_t reg = p[0] >> 24, nv = p[0] & 0x00FFFFFFu, ov = g.bp[reg];
        if (ov == nv) continue;
        g.bp[reg] = nv;
        cls |= kBpMark.t[reg];
        const uint64_t zn = kCompact ? KeyZ(reg, nv) : (static_cast<uint64_t>(p[1]) << 32) | p[2];
        kh ^= KeyZCached(reg, ov) ^ zn;
        KeyZRemember(reg, nv, zn);
    }
    for (q = p + n[kDBp]; p < q; ++p) {
        const uint32_t reg = p[0] >> 24, nv = p[0] & 0x00FFFFFFu;
        if (g.bp[reg] == nv) continue;
        g.bp[reg] = nv;
        cls |= kBpMark.t[reg];
    }
    for (q = p + n[kDBpMask] * 2; p < q; p += 2) {
        const uint32_t reg = p[0] >> 24, ov = g.bp[reg], nv = (ov & p[1]) | (p[0] & 0x00FFFFFFu);
        if (ov == nv) continue;
        g.bp[reg] = nv;
        const uint32_t c = kBpMark.t[reg];
        cls |= c;
        if (c & kMarkKey) {
            const uint64_t zn = KeyZCached(reg, nv);
            kh ^= KeyZCached(reg, ov) ^ zn;
            KeyZRemember(reg, nv, zn);
        }
    }
    for (q = p + n[kDTex]; p < q; ++p) { // (WriteBP's texture-unit path)
        const uint32_t reg = p[0] >> 24, nv = p[0] & 0x00FFFFFFu, ov = g.bp[reg];
        if (ov == nv) continue;
        g.bp[reg] = nv;
        LodRegChanged(reg, ov, nv);
        if (const uint8_t bit = kBpPlain.tex[reg]) {
            g.texDirty = true;
            g.texSlotDirty |= bit;
            g.bpTexStale |= bit;
        }
    }
    for (q = p + n[kDTev]; p < q; ++p) {
        // (kind 6's test, then WriteTevColor; its bp[] store is TevBp's below)
        const uint32_t slot = p[0] >> 24, nv = p[0] & 0x00FFFFFFu;
        if (g.tevRaw[slot] == nv) continue;
        const uint32_t reg = 0xE0u + (slot >> 1), idx = slot >> 2;
        const bool konst = (nv & 0x800000u) != 0;
        int* const d = (konst ? g.tevKonst : g.tevReg)[idx];
        const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
        const uint8_t bit = static_cast<uint8_t>(1u << (idx + (konst ? 4 : 0)));
        if (reg & 1) { if (d[2] != lo || d[1] != hi) { d[2] = lo; d[1] = hi; g.tevColDirty |= bit; } } // BG
        else { if (d[0] != lo || d[3] != hi) { d[0] = lo; d[3] = hi; g.tevColDirty |= bit; } }         // RA
        if (!konst) TevOorUpdate(static_cast<int>(idx));
        g.tevRaw[slot] = nv;
    }
    for (q = p + n[kDTevBp]; p < q; ++p) g.bp[p[0] >> 24] = p[0] & 0x00FFFFFFu;
    for (q = p + n[kDXfWKey] * kXfKeyRec; p < q; p += kXfKeyRec) { // (kind 7's path, the key hash collected)
        uint32_t& d = g.xfReg[p[0]];
        if (d == p[1]) continue;
        const uint32_t o = d;
        d = p[1];
        const uint64_t zn = kCompact ? KeyZ(0x100u + p[0], p[1]) : (static_cast<uint64_t>(p[2]) << 32) | p[3];
        kh ^= KeyZCached(0x100u + p[0], o) ^ zn;
        KeyZRemember(0x100u + p[0], p[1], zn);
        MarkXFRegFlags(p[0]);
    }
    for (q = p + n[kDXfW] * 2; p < q; p += 2) {
        uint32_t& d = g.xfReg[p[0]];
        if (d != p[1]) { const uint32_t o = d; d = p[1]; MarkXFReg(p[0], o, p[1]); }
    }
    if (kPart == 1) goto flags;
eager:
    for (uint32_t i = 0; i < n[kDXfB]; ++i) {
        const uint32_t addr = p[0] & 0xFFFFu, cnt = p[0] >> 16;
        if (cnt > 1) WriteXF(addr, cnt, p + 1);
        else WriteXFWord(addr, p[1]);
        p += 1 + cnt;
    }
    for (q = p + n[kDCp] * 2; p < q; p += 2) WriteCPInline(static_cast<uint8_t>(p[0]), p[1]);
    if (kPart == 2) return;
flags:
    if (cls) {
        if (cls & kMarkKey) g.bpKeyDirty = true;
        if (cls & (kMarkKey | kMarkTev)) g.bpDirty = true;
        if (cls & kMarkRs) g.bpRsDirty = true;
    }
    if (kh && RsCacheMode() != 0) g.keyHash ^= kh;
}
void SlDeltaApply(const SlEntry& e) {
    if (e.delta0 == 4) { ++g_slCompactApplied; SlDeltaApplyT<true>(e); }
    else SlDeltaApplyT<false>(e);
}

inline int SlDeltaMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_SLDELTA", 1);
        return v == 2 && !diag::kEnabled ? 1 : static_cast<int>(v < 0 || v > 2 ? 1 : v);
    }()));
    return m;
}
uint64_t g_slDeltaReplays = 0, g_slDeltaChecked = 0, g_slDeltaBad = 0, g_slDeltaCompiled = 0, g_slDeltaRefused = 0;
// SlReplay, or the list's register deltas where they apply (see above).
inline void SlRun(SlEntry& e, int bpFast) {
    const int mode = SlDeltaMode();
    if (e.delta0 == 0 && mode != 0 && CurrentFrame() != e.parseFrame) {
        e.delta0 = SlCompileDelta(e) ? (SlHotCompact() ? 4 : 2) : 3;
        ++(e.delta0 != 3 ? g_slDeltaCompiled : g_slDeltaRefused);
    }
    if ((e.delta0 == 2 || e.delta0 == 4) && bpFast == 1 && g.bpMask == 0x00FFFFFFu && RegShadow() == 1) {
        if (mode == 1) {
            ++g_slDeltaReplays;
            SlDeltaApply(e);
            return;
        }
        if (diag::kEnabled && mode == 2) {
            static State before, byDelta; // (static: the state is large for a thread's stack)
            std::memcpy(&before, &g, sizeof(State));
            SlDeltaApply(e);
            std::memcpy(&byDelta, &g, sizeof(State));
            std::memcpy(&g, &before, sizeof(State));
            SlReplay(e, bpFast);
            ++g_slDeltaChecked;
            if (std::memcmp(&byDelta, &g, sizeof(State)) != 0) {
                ++g_slDeltaBad;
                VGX_LOG_N(16, "sldelta: MISMATCH (%u bytes)\n", e.nbytes);
            }
            return;
        }
    }
    SlReplay(e, bpFast);
}
} // namespace
namespace {
// ------------------------------------------------------- mixed display lists: exact plans (GXV_FEPLAN, feplan31)
// A list that holds state commands AND draws (or a CALL_DL, which a list ignores, or an opcode the parser stops at) is not a state
// list (SlParse refuses it) and not a geometry entry (vgx_dlcache.cpp Build refuses it): until now every call of it went to
// ProcessStream, the opcode walk with its per-command tests. Its plan keeps the list as ProcessStream would walk it: state sections
// (SlParse's operations, each section its own SlEntry, so replayed by SlRun: SlReplay or the section's register deltas, GXV_SLDELTA),
// and between them, in list order, the barriers: draws, CALL_DLs, the unknown opcode. A draw is made at the replay exactly as the
// parse makes it: the vertex plan of its format (GetPlan, now), the same source pointer into the list, count and room left
// ("avail"), DrawStreamPlan, the same counters. What a parse decides from the state rather than from the bytes is the length of a
// draw (its vertex size: the vertex descriptor and the format's attribute table); the plan keeps the size it was made with, and a
// replay that finds another size at a draw parses the rest of the list from that draw with ProcessStream (the state up to there is
// the parse's, and the parse from a command boundary to the end is the parse's own) and makes the plan again at the next call. A list
// that writes the vertex descriptor or a format table (CP 0x50..0x9F) before a draw has no plan (its draw sizes are not known before
// the replay): it is parsed as before. Validity is the state list's: the write generation of the list's bytes or, after a write near
// them, the bytes compared with the copy kept at the parse (GXV_SLBYTES), the same churn back-off.
//   GXV_FEPLAN=0 (default) off: such lists are parsed every call, as before
//   GXV_FEPLAN=1 on
//   GXV_FEPLAN=2 verify (diagnostics builds; a quiet build takes it as 0): the list is parsed for real, every draw it makes recorded with the
//                whole register state (State) before and after it; then, from the state before the call, the plan is run with its
//                draws checked instead of made: each draw's opcode, source, count and room, and the State before it, must be the parse's
//                (then the State after the parse's draw is put in place); the State the plan leaves must be the parse's. A difference is
//                logged ("feplan: MISMATCH ...") and the parse's State kept. Lists with a trigger (draw done, token, EFB copy, palette load,
//                marker) are not compared (it would run twice). Summary every 2^14 checks and on the census line.
// Not with the two-core front end or the J3D-native hooks (GXV_FE_TWOCORE, GXV_J3DNATIVE: lists are parsed as before), not in the second
// worker. Counters: FePlanCounters.
struct MixStep {
    uint8_t kind;   // 0 state section secs[sec], 1 draw, 2 CALL_DL inside the list (ignored, as ProcessStream does), 3 unknown opcode (the parse stops)
    uint8_t cmd;    // draw / unknown: the opcode
    uint16_t sec;
    uint32_t off;   // draw: where its vertex data starts in the list (after opcode and count); unknown: the bytes left after the opcode
    uint32_t count; // draw: vertices
    uint32_t vsize; // draw: the vertex size the plan was made with
};
struct MixPlan {
    std::vector<MixStep> steps;
    std::vector<std::unique_ptr<SlEntry>> secs;
    uint32_t stateCmds = 0;   // (statistics: what the parse counts)
    bool trigger = false;     // a BP write with an effect outside the registers (verify does not compare such a list)
};
void MixPlanDel::operator()(MixPlan* m) const { delete m; }

uint64_t g_fpBuilds = 0, g_fpRefusedFmt = 0, g_fpReplays = 0, g_fpFallbacks = 0, g_fpChecked = 0, g_fpBad = 0, g_fpUncompared = 0, g_fpTriggers = 0;
inline int FePlanMode() {
#if defined(VGX_CLONE_B)
    return 0;
#else
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_FEPLAN", 0);
        if (v == 2) return diag::kEnabled ? 2 : 0; // (verify needs a diagnostics build; a quiet build stays off rather than on unverified)
        return v == 1 ? 1 : 0;
    }()));
    return m;
#endif
}
// The vertex size of format `fmt` from the registers as they are, without touching the player's plan cache (GetPlan's answer: the
// same function of the descriptor and the format table).
uint32_t FpStreamSize(int fmt) {
    static PlanCache scratch;
    bool dirty = true;
    return GetPlanIn(scratch, dirty, g.vcd, g.vat, fmt, false).streamSize;
}
inline bool FpVtxFmtReg(uint8_t reg) { return reg >= 0x50 && reg < 0xA0; } // (CP: the descriptor and the format tables)

// The plan of a list SlParse refused; false (no plan): a vertex-format write before a draw, or no state command at all. The walk is
// ProcessStream's for a list.
bool MixBuild(SlEntry& e, const uint8_t* list, uint32_t nbytes, uint64_t frame) {
    std::unique_ptr<MixPlan, MixPlanDel> m(new MixPlan);
    const bool shadow = RegShadow() != 0;
    const uint8_t* p = list;
    const uint8_t* const end = list + nbytes;
    SlEntry* cur = nullptr;
    bool fmtWritten = false;
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) {
            while (end - p >= 4) {
                uint32_t w;
                std::memcpy(&w, p, 4);
                if (w) break;
                p += 4;
            }
            continue;
        }
        if (cmd == 0x44 || cmd == 0x48) continue;
        if (cmd == 0x61 || cmd == 0x08 || cmd == 0x10 || cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (!cur) {
                if (m->secs.size() >= 0xFFFF) return false;
                m->secs.emplace_back(new SlEntry);
                cur = m->secs.back().get();
                cur->usable = true;
                cur->live = true;
                cur->parseFrame = frame;
                m->steps.push_back(MixStep{0, 0, static_cast<uint16_t>(m->secs.size() - 1), 0, 0, 0});
            }
            if (cmd == 0x08 && end - p >= 5 && FpVtxFmtReg(p[0])) fmtWritten = true;
            const size_t ops0 = cur->ops.size();
            const int r = SlParseCmd(*cur, cmd, p, end, shadow);
            if (r == 0) break; // (cut by the end: the parse stops there)
            ++m->stateCmds;
            if (cur->ops.size() > ops0 && cur->ops.back().kind == 1 && (cur->ops.back().v >> 24) != 0xFE) m->trigger = true;
            continue;
        }
        if (cmd == 0x40) {
            if (end - p < 8) break;
            p += 8;
            cur = nullptr;
            m->steps.push_back(MixStep{2, cmd, 0, 0, 0, 0});
            continue;
        }
        if (cmd & 0x80) {
            if (end - p < 2) break;
            if (fmtWritten) { ++g_fpRefusedFmt; return false; }
            const uint32_t count = be16(p);
            const uint32_t vsize = FpStreamSize(cmd & 7);
            const uint32_t avail = static_cast<uint32_t>(end - p) - 2u;
            const uint64_t used = static_cast<uint64_t>(count) * vsize;
            p += 2;
            cur = nullptr;
            m->steps.push_back(MixStep{1, cmd, 0, static_cast<uint32_t>(p - list), count, vsize});
            if (used > avail) break;
            p += used;
            continue;
        }
        m->steps.push_back(MixStep{3, cmd, 0, static_cast<uint32_t>(end - p), 0, 0});
        break;
    }
    // (a list of draws (and calls) only: no state section, nothing a plan saves over the parse but its walk; left to the parser, as before.
    // The census counts such lists as "draws only")
    if (m->stateCmds == 0) return false;
#if defined(GXV_FEPLAN_MUTANT) && GXV_FEPLAN_MUTANT == 1
    // (test-only negative control: the last state operation of every section after the first draw is left out; the tests must see it)
    {
        bool drawn = false;
        for (const MixStep& st : m->steps) {
            if (st.kind == 1) drawn = true;
            if (st.kind == 0 && drawn && !m->secs[st.sec]->ops.empty()) m->secs[st.sec]->ops.pop_back();
        }
    }
#endif
    e.mix = std::move(m);
    ++g_fpBuilds;
    return true;
}

} // namespace
// The State the parse records around its draws (GXV_FEPLAN=2)
struct FpRec {
    struct D { uint8_t cmd; const uint8_t* src; uint32_t count, avail; };
    std::vector<D> d;
    std::vector<std::unique_ptr<State>> before, after;
    size_t n = 0;
};
static void FpRecBefore(uint8_t cmd, const uint8_t* src, uint32_t count, uint32_t avail) {
    FpRec& r = *g_fpRec;
    if (r.before.size() <= r.n) { r.before.emplace_back(new State); r.after.emplace_back(new State); }
    if (r.d.size() <= r.n) r.d.resize(r.n + 1);
    r.d[r.n] = FpRec::D{cmd, src, count, avail};
    std::memcpy(r.before[r.n].get(), &g, sizeof(State));
}
static void FpRecAfter() {
    FpRec& r = *g_fpRec;
    std::memcpy(r.after[r.n].get(), &g, sizeof(State));
    ++r.n;
}
namespace {

// Run the plan of e (the list's bytes are the plan's). chk null: for real; 0 done, 1 a draw's vertex size differed (the rest parsed by
// ProcessStream; the plan is to be made again). chk set (GXV_FEPLAN=2): draws checked against the parse's record instead of made:
// 0 equal so far (the caller compares the State left), 2 a difference (*at: the draw, *what: 1 opcode / source / count / room,
// 2 State before it, 3 the parse made fewer draws, 4 more), 3 a vertex size differed (not compared).
int MixRun(SlEntry& e, const uint8_t* list, uint32_t nbytes, int bpFast, const FpRec* chk, uint32_t* at, int* what) {
    MixPlan& m = *e.mix;
    stats::Frame& fr = stats::Cur();
    const uint32_t s0 = fr.streams, sb0 = fr.streamBytes;
    size_t di = 0;
    int result = 0;
    for (const MixStep& st : m.steps) {
        if (st.kind == 0) {
            SlRun(*m.secs[st.sec], bpFast);
            continue;
        }
        if (st.kind == 1) {
            const Plan& plan = GetPlan(st.cmd & 7);
            const uint32_t vsize = plan.streamSize;
#if !(defined(GXV_FEPLAN_MUTANT) && GXV_FEPLAN_MUTANT == 2) // (test-only negative control: the size is not checked)
            if (vsize != st.vsize) {
                if (chk) { result = 3; break; }
                fr.streams = s0;
                fr.streamBytes = sb0;
                ++fr.streams;
                fr.streamBytes += nbytes;
                ++g_sd.calls[1];
                g_sd.bytes[1] += nbytes;
                const uint32_t from = st.off - 3u; // (the draw's opcode)
                ProcessStreamT<false>(list + from, nbytes - from, true, true);
                return 1;
            }
#endif
            const uint32_t avail = nbytes - st.off;
            const uint64_t used = static_cast<uint64_t>(st.count) * vsize;
            if (used > avail && !chk) VGX_LOG_N(16, "draw in a display list runs past its end: %u verts x %u bytes > %u\n", st.count, vsize, avail);
            if (vsize != 0 && st.count != 0) {
                if (chk) {
                    if (di >= chk->n) { *at = static_cast<uint32_t>(di); *what = 3; result = 2; break; }
                    const FpRec::D& d = chk->d[di];
                    if (d.cmd != st.cmd || d.src != list + st.off || d.count != st.count || d.avail != avail) { *at = static_cast<uint32_t>(di); *what = 1; result = 2; break; }
                    if (std::memcmp(chk->before[di].get(), &g, sizeof(State)) != 0) { *at = static_cast<uint32_t>(di); *what = 2; result = 2; break; }
                    std::memcpy(&g, chk->after[di].get(), sizeof(State));
                    ++di;
                } else {
                    ++g_sd.draws[1];
                    g_sd.verts[1] += st.count;
                    DrawStreamPlan(st.cmd & 0xF8, st.cmd & 7, plan, list + st.off, st.count, avail, 1);
                    ++fr.primsDL;
                    fr.vertsDL += st.count;
                }
            }
            continue;
        }
        if (chk) continue;
        if (st.kind == 2) VGX_LOG_N(8, "CALL_DL inside a display list ignored\n");
        else VGX_LOG_N(16, "unknown GX opcode 0x%02X in a %s (%u bytes left)\n", st.cmd, "display list", st.off);
    }
    if (chk) {
        if (result == 0 && di != chk->n) { *at = static_cast<uint32_t>(di); *what = 4; result = 2; }
        fr.streams = s0;
        fr.streamBytes = sb0;
        return result;
    }
    // (ProcessStream's statistics for the call: one stream of nbytes, the list counters)
    fr.streams = s0 + 1;
    fr.streamBytes = sb0 + nbytes;
    ++g_sd.calls[1];
    g_sd.bytes[1] += nbytes;
    g_sd.stateCmds[1] += m.stateCmds;
    return 0;
}

// ------------------------------------------------------- the census of lists parsed again (GXV_FEPLAN_CENSUS, feplan31)
// Count only (default 0; any build): every display-list call that ends in a full parse (ProcessStream) is timed and sorted by the
// way it got there (vgx_dlcache.cpp CallListBody: tiny, not guest RAM, bypassed after Build refused it, bypassed for rewrites or a
// variant streak, refused now, cache off / traced picture) and by what the list holds (the barriers that keep it from being a state
// list: draws, CALL_DLs, an unknown opcode; or nothing: state only / draws only), with its bytes, state commands and draws; the plan
// replays (GXV_FEPLAN=1) are timed the same way. Every 300 pictures (GXV_FEPLAN_CENSUS=<n> >= 2: every n) two "feplan-census:" lines (port_log_summary): per picture, the
// lists by way and by barrier (count, ms), what a plan could take (state with barriers, not tiny, guest RAM, no vertex-format write
// before a draw), the plans' own counters, and the eight costliest lists (address, bytes, way, barrier, calls, ms, first bytes).
// The time is the parse only (the classification is outside the timed part); the timer's own cost (one clock read pair a list) is in it.
enum : uint8_t { FpTiny, FpNotGuest, FpBypassState, FpBypassOther, FpRefusedNow, FpBypassSetNow, FpOff, FpPlan, FpPaths };
enum : uint8_t { FcState, FcDraw, FcStateDraw, FcStateCall, FcStateDrawCall, FcDrawCall, FcUnknown, FcEmpty, FcClasses };
struct FpShape {
    uint32_t stateCmds = 0, draws = 0, verts = 0, calls = 0, stateBytes = 0, drawBytes = 0;
    uint8_t cls = FcEmpty;
    bool fmtBeforeDraw = false, unknown = false;
};
FpShape FpClassify(const uint8_t* list, uint32_t nbytes) {
    FpShape s;
    const uint8_t* p = list;
    const uint8_t* const end = list + nbytes;
    bool fmt = false;
    while (p < end) {
        const uint8_t* const c0 = p;
        const uint8_t cmd = *p++;
        if (cmd == 0x00 || cmd == 0x44 || cmd == 0x48) continue;
        size_t len = 0;
        if (cmd == 0x61) len = 4;
        else if (cmd == 0x08) { len = 5; if (end - p >= 5 && FpVtxFmtReg(p[0])) fmt = true; }
        else if (cmd == 0x10) { if (end - p < 4) break; len = 4u + ((be16(p) & 0xFu) + 1u) * 4u; }
        else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) len = 4;
        else if (cmd == 0x40) { if (end - p < 8) break; p += 8; ++s.calls; continue; }
        else if (cmd & 0x80) {
            if (end - p < 2) break;
            const uint32_t count = be16(p);
            const uint64_t used = static_cast<uint64_t>(count) * FpStreamSize(cmd & 7);
            ++s.draws;
            s.verts += count;
            if (fmt) s.fmtBeforeDraw = true;
            const uint64_t room = static_cast<uint64_t>(end - p) - 2u;
            s.drawBytes += static_cast<uint32_t>(3u + (used < room ? used : room));
            if (used > room) break;
            p += 2 + used;
            continue;
        } else { s.unknown = true; break; }
        if (static_cast<size_t>(end - p) < len) break;
        p += len;
        ++s.stateCmds;
        s.stateBytes += static_cast<uint32_t>(p - c0);
    }
    if (s.unknown) s.cls = FcUnknown;
    else if (s.stateCmds) s.cls = s.draws ? (s.calls ? FcStateDrawCall : FcStateDraw) : (s.calls ? FcStateCall : FcState);
    else s.cls = s.draws ? (s.calls ? FcDrawCall : FcDraw) : FcEmpty;
    return s;
}
struct FpAcc { uint64_t n = 0, ns = 0, bytes = 0, stateCmds = 0, draws = 0; };
struct FpTop { const uint8_t* list = nullptr; uint32_t nbytes = 0, calls = 0, stateCmds = 0, draws = 0; uint64_t ns = 0; uint8_t path = 0, cls = 0; uint8_t head[8] = {}; };
struct FpCensus {
    uint64_t first = 0, last = 0, lost = 0;
    FpAcc path[FpPaths], cls[FcClasses], eligible, fmtBlocked, tinyMixed;
    FpTop top[256];
    uint64_t c0[8] = {}; // (the plans' counters at the window's start)
} g_fpc;
inline bool FpEligible(uint8_t path, const FpShape& s) {
    return (path == FpBypassState || path == FpBypassOther || path == FpRefusedNow || path == FpBypassSetNow) && s.stateCmds != 0 &&
           (s.draws != 0 || s.calls != 0 || s.unknown) && !s.fmtBeforeDraw;
}
void FpCensusReport();
// (GXV_FEPLAN_CENSUS=1: lines every 300 pictures; =<n>, n >= 2: every n pictures)
inline uint64_t FpCensusWindow() {
    const int n = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_FEPLAN_CENSUS", 0))));
    return n >= 2 ? static_cast<uint64_t>(n) : 300u;
}
void FpCensusNote(const uint8_t* list, uint32_t nbytes, uint8_t path, uint64_t ns, const FpShape& s) {
    const uint64_t frame = CurrentFrame();
    if (frame != g_fpc.last) {
        if (!g_fpc.first) g_fpc.first = frame;
        g_fpc.last = frame;
        if (frame - g_fpc.first >= FpCensusWindow()) FpCensusReport();
    }
    auto add = [&](FpAcc& a) { ++a.n; a.ns += ns; a.bytes += nbytes; a.stateCmds += s.stateCmds; a.draws += s.draws; };
    add(g_fpc.path[path < FpPaths ? path : FpOff]);
    add(g_fpc.cls[s.cls < FcClasses ? s.cls : FcEmpty]);
    if (path != FpPlan) {
        if (FpEligible(path, s)) add(g_fpc.eligible);
        else if (s.fmtBeforeDraw && s.stateCmds) add(g_fpc.fmtBlocked);
        if (path == FpTiny && s.stateCmds && s.draws) add(g_fpc.tinyMixed);
    }
    uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(list) >> 5) * 0x9E3779B1u) >> 24; // (8 bits: 256 slots)
    for (uint32_t k = 0; k < 8; ++k, h = (h + 1) & 255u) {
        FpTop& t = g_fpc.top[h];
        if (t.list && !(t.list == list && t.path == path)) continue;
        if (!t.list) {
            t.list = list;
            t.path = path;
            std::memcpy(t.head, list, nbytes < 8 ? nbytes : 8);
        }
        t.nbytes = nbytes;
        t.cls = s.cls;
        t.stateCmds = s.stateCmds;
        t.draws = s.draws;
        ++t.calls;
        t.ns += ns;
        return;
    }
    ++g_fpc.lost;
}
void FpCensusReport() {
    FpCensus& c = g_fpc;
    const uint64_t pics = c.last - c.first;
    const uint64_t now[8] = {g_fpReplays, g_fpBuilds, g_fpRefusedFmt, g_fpFallbacks, g_fpChecked, g_fpBad, g_fpUncompared, g_fpTriggers};
    if (port_log_summary && pics) {
        static const char* const kPath[FpPaths] = {"tiny", "not guest RAM", "bypassed (Build refused)", "bypassed (rewrites/streak)", "refused now",
                                                   "bypass set now", "cache off/traced", "plan replays"};
        static const char* const kCls[FcClasses] = {"state only", "draws only", "state+draw", "state+CALL", "state+draw+CALL", "draw+CALL", "unknown opcode", "empty"};
        const double p = static_cast<double>(pics);
        FpAcc all;
        for (int i = 0; i < FpPaths; ++i) {
            if (i == FpPlan) continue;
            all.n += c.path[i].n; all.ns += c.path[i].ns; all.bytes += c.path[i].bytes; all.stateCmds += c.path[i].stateCmds; all.draws += c.path[i].draws;
        }
        char bp[520], bc[520];
        int ap = 0, ac = 0;
        bp[0] = bc[0] = 0;
        for (int i = 0; i < FpPaths; ++i)
            if (c.path[i].n && ap < static_cast<int>(sizeof bp) - 80)
                ap += std::snprintf(bp + ap, sizeof bp - ap, " %s %.1f (%.3f ms, %.0f state cmds, %.1f draws);", kPath[i], c.path[i].n / p, c.path[i].ns / 1e6 / p,
                                    c.path[i].stateCmds / p, c.path[i].draws / p);
        for (int i = 0; i < FcClasses; ++i)
            if (c.cls[i].n && ac < static_cast<int>(sizeof bc) - 80)
                ac += std::snprintf(bc + ac, sizeof bc - ac, " %s %.1f (%.3f ms, %.0f state cmds);", kCls[i], c.cls[i].n / p, c.cls[i].ns / 1e6 / p, c.cls[i].stateCmds / p);
        auto d = [&](int i) { return (now[i] - c.c0[i]) / p; };
        port_log_summary("feplan-census: %llu pictures (GXV_FEPLAN=%d); lists parsed again per picture %.1f (%.1f KB, %.0f state cmds, %.1f draws, %.3f ms); by way:%s by barrier "
                         "(plan replays included):%s a plan could take %.1f (%.0f state cmds, %.1f draws, %.3f ms); vertex format before a draw %.1f (%.3f ms); tiny with state and "
                         "draws %.1f (%.3f ms); plans per picture: replays %.1f, made %.2f, refused (vertex format) %.2f, vertex size changed %.2f; verify: checked %.1f, mismatching "
                         "%.2f, not compared (size changed %.2f, trigger %.2f)",
                         static_cast<unsigned long long>(pics), FePlanMode(), all.n / p, all.bytes / 1024.0 / p, all.stateCmds / p, all.draws / p, all.ns / 1e6 / p,
                         ap ? bp : " -", ac ? bc : " -", c.eligible.n / p, c.eligible.stateCmds / p, c.eligible.draws / p, c.eligible.ns / 1e6 / p, c.fmtBlocked.n / p,
                         c.fmtBlocked.ns / 1e6 / p, c.tinyMixed.n / p, c.tinyMixed.ns / 1e6 / p, d(0), d(1), d(2), d(3), d(4), d(5), d(6), d(7));
        FpTop* top[8] = {};
        for (FpTop& s : c.top) {
            if (!s.list || !s.calls) continue;
            for (int i = 0; i < 8; ++i)
                if (!top[i] || s.ns > top[i]->ns) {
                    for (int k = 7; k > i; --k) top[k] = top[k - 1];
                    top[i] = &s;
                    break;
                }
        }
        static const char* const kPathShort[FpPaths] = {"tiny", "notguest", "bypass-build", "bypass-rw", "refused", "bypass-set", "off", "plan"};
        static const char* const kClsShort[FcClasses] = {"state", "draws", "state+draw", "state+call", "state+draw+call", "draw+call", "unknown", "empty"};
        char line[1100];
        int at = 0;
        for (int i = 0; i < 8 && top[i]; ++i) {
            const FpTop& s = *top[i];
            at += std::snprintf(line + at, sizeof line - at, " [%p %uB %s %s %.2f/pic %.3fms %.1fus %u/%u %02X%02X%02X%02X%02X%02X%02X%02X]", static_cast<const void*>(s.list),
                                s.nbytes, kPathShort[s.path < FpPaths ? s.path : 0], kClsShort[s.cls < FcClasses ? s.cls : 0], s.calls / p, s.ns / 1e6 / p,
                                s.ns / 1e3 / s.calls, s.stateCmds, s.draws, s.head[0], s.head[1], s.head[2], s.head[3], s.head[4], s.head[5], s.head[6], s.head[7]);
            if (at >= static_cast<int>(sizeof line) - 140) break;
        }
        port_log_summary("feplan-census: costliest lists (address, bytes, way, barrier, calls a picture, ms a picture, us a call, state cmds/draws a call, first bytes):%s%s",
                         at ? line : " none", c.lost ? " (table full: some not listed)" : "");
    }
    const uint64_t keepLast = c.last;
    c = FpCensus{};
    c.first = c.last = keepLast;
    for (int i = 0; i < 8; ++i) c.c0[i] = now[i];
}
inline bool FpCensusOnK() { return VGX_KNOB(EnvInt("GXV_FEPLAN_CENSUS", 0) != 0) != 0; }
} // namespace
bool FePlanCensusOn() { return FpCensusOnK(); }
void FePlanCensusParse(const uint8_t* list, uint32_t nbytes, uint8_t path) {
    const FpShape s = FpClassify(list, nbytes);
    const int64_t t0 = port_host_time_ns ? port_host_time_ns() : 0;
    ProcessStream(list, nbytes, true);
    const uint64_t ns = port_host_time_ns ? static_cast<uint64_t>(port_host_time_ns() - t0) : 0;
    FpCensusNote(list, nbytes, path, ns, s);
}
void FePlanCounters(uint64_t out[8]) {
    out[0] = g_fpReplays; out[1] = g_fpBuilds; out[2] = g_fpRefusedFmt; out[3] = g_fpFallbacks; out[4] = g_fpChecked; out[5] = g_fpBad; out[6] = g_fpUncompared; out[7] = g_fpTriggers;
}
namespace {
// A mixed list's plan run for StateListCall (e valid, e.mix set): true when handled (always, unless the switch is off).
bool MixCall(SlEntry& e, const uint8_t* list, uint32_t nbytes, int bpFast, int mode) {
    if (diag::kEnabled && mode == 2) {
        // the parse for real, its draws recorded; then the plan, checked against it, from the same start; the parse's State kept
        static State before, byParse;
        static FpRec rec;
        std::memcpy(&before, &g, sizeof(State));
        rec.n = 0;
        g_fpRec = &rec;
        ProcessStream(list, nbytes, true);
        g_fpRec = nullptr;
        if (e.mix->trigger) { ++g_fpTriggers; return true; }
        std::memcpy(&byParse, &g, sizeof(State));
        std::memcpy(&g, &before, sizeof(State));
        uint32_t at = 0;
        int what = 0;
        int r = MixRun(e, list, nbytes, bpFast, &rec, &at, &what);
        if (r == 0 && std::memcmp(&g, &byParse, sizeof(State)) != 0) { r = 2; what = 5; at = static_cast<uint32_t>(rec.n); }
        std::memcpy(&g, &byParse, sizeof(State));
        if (r == 3) { ++g_fpUncompared; e.live = false; return true; }
        ++g_fpChecked;
        if (r == 2) {
            ++g_fpBad;
            static const char* const kWhat[6] = {"?", "a draw's opcode / source / count / room", "the State before a draw", "the parse made fewer draws",
                                                 "the parse made more draws", "the State after the list"};
            VGX_LOG_N(16, "feplan: MISMATCH for list %p (%u bytes): %s (draw %u of %u)\n", static_cast<const void*>(list), nbytes, kWhat[what < 6 ? what : 0], at,
                      static_cast<unsigned>(rec.n));
        }
        if ((g_fpChecked & 0x3FFFu) == 0)
            VGX_LOG_N(100, "feplan: %llu plans checked, %llu mismatching\n", static_cast<unsigned long long>(g_fpChecked), static_cast<unsigned long long>(g_fpBad));
        return true;
    }
    const bool census = FpCensusOnK();
    const int64_t t0 = census && port_host_time_ns ? port_host_time_ns() : 0;
    ++g_fpReplays;
    if (MixRun(e, list, nbytes, bpFast, nullptr, nullptr, nullptr) == 1) {
        ++g_fpFallbacks;
        e.live = false; // (made again at the next call)
        if (++e.mixMiss >= 3) {
            // (a list called with vertex formats that keep changing: parsed as before for a few seconds, as a churning state list)
            e.mixMiss = 0;
            e.quietUntil = CurrentFrame() + 300;
            e.quietWhy = 4;
        }
    } else if (e.mixMiss) {
        e.mixMiss = 0;
    }
    if (census) {
        const uint64_t ns = port_host_time_ns ? static_cast<uint64_t>(port_host_time_ns() - t0) : 0;
        FpCensusNote(list, nbytes, FpPlan, ns, FpClassify(list, nbytes));
    }
    return true;
}
} // namespace

uint64_t g_slFastHits = 0; // (StateListFastHit's replays, for the "fe cuts" line)
void FeCutCounters(uint64_t out[4]) { out[0] = g_slFastHits; out[1] = g_slCompactApplied; out[2] = out[3] = 0; }
void SlDeltaCounters(uint64_t out[5]) {
    out[0] = g_slDeltaReplays; out[1] = g_slDeltaChecked; out[2] = g_slDeltaBad; out[3] = g_slDeltaCompiled; out[4] = g_slDeltaRefused;
}

// The common case without the map: the list was parsed before, is state-only and its bytes have not been written since.
// GXV_SLFAST=1 (default) also takes, without the geometry cache's detour, the two cases StateListCall would replay anyway:
//   - a list whose address class in the front holds another list: its entry is looked up in the table (never created here);
//   - a list whose memory was written since, with the bytes it held (J3D writes its materials' lists again on drawn turns,
//     GXV_DL_LAZY): compared with the copy kept at the parse, exactly StateListCall's "a write near it, not on it" test.
// Anything else (no entry, another size, parsed as not state-only, backed off, really changed) returns false, and CallList goes
// on as before; so a list is replayed here only where StateListCall would replay it, and with the same SlReplay.
// (Replaying a state-only list instead of the geometry cache's attempt is what that cache does itself: a list with a state
// command is never one of its entries, vgx_dlcache.cpp Build.)
#if defined(VGX_CLONE_B)
// GXV_FE_TWOCORE, the second worker in a segment: a list with a trigger (draw done, token, EFB copy, marker) is the player's to replay.
inline bool SlHard(const SlEntry& e) {
    if (!two::g_decoding) return false;
    for (const SlOp& op : e.ops)
        if (op.kind == 1) {
            const uint32_t r = op.v >> 24;
            if (r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0xFF) return true;
        }
    return false;
}
#endif
bool StateListFastHit(const uint8_t* list, uint32_t nbytes) {
    const int mode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_STATELIST", 1))));
    if (mode != 1) return false;
    const bool wide = SlFastMode() != 0;
    SlFront& f = SlFrontOf(list);
    SlEntry* ep = f.list == list ? f.e : nullptr;
    bool fromTable = false;
    if (!ep) {
        if (!wide) return false;
        ep = SlLookup(list);
        if (!ep) return false;
        fromTable = true;
    }
    SlEntry& e = *ep;
    if (!e.usable || e.nbytes != nbytes || !e.live) return false;
    // (GXV_FECUT_HINT: the delta the replay reads next, while the generation counter is looked up; see FeCutHint)
    if (FeCutHint() && (e.delta0 == 2 || e.delta0 == 4)) FePrefetchLines(e.dp, e.dbytes, 24);
    const uint64_t frame = CurrentFrame();
    if (frame < e.quietUntil) return false;
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    bool same = false;
    if (gen != e.gen) {
        if (!wide || gen == kNotGuest || !SlKeepBytes() || e.bytes.size() != nbytes) return false;
        // (the generation is read before the bytes, as the geometry cache reads them: a write that lands after this moves it again)
        std::atomic_thread_fence(std::memory_order_acquire);
        if (FeCutHint()) { FePrefetchLines(list, nbytes, 8); FePrefetchLines(e.bytes.data(), nbytes, 8); } // (GXV_FECUT_HINT: the first lines of both)
        if (!SameBytes(list, e.bytes.data(), nbytes)) return false;
        e.gen = gen; // (StateListCall's "a write near it, not on it")
        same = true;
    }
    if (fromTable) { f.list = list; f.e = &e; } // (as StateListCall's lookup leaves the front)
    const int bpFastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1))));
    // (as `changes = 0; lastFrame = frame;` were: stores only where they change something, so that a hit leaves the entry's line clean)
    if (e.changes) e.changes = 0;
    if (frame == 0) e.live = false;
    ++g_slHits;
    ++g_slFastHits;
    if (fromTable) ++g_slFastTable; else ++g_slFastFront;
    if (same) ++g_slFastSame;
#if defined(VGX_CLONE_B)
    if (SlHard(e)) return false;
#endif
    SlRun(e, bpFastMode == 2 ? 1 : bpFastMode);
    return true;
}

// GXV_FE_PREFETCH (see "look-ahead" before ProcessStream): what StateListFastHit / StateListCall will read for this list, hinted.
// 1: the front slot, the table's home slot (StateListFastHit looks there on a front miss, also for every geometry list) and the
// generation counter; 2: the entry; 3: its operations and words; 4: when the list's memory was written since (J3D's lists on drawn
// turns), the list and the copy SameBytes compares it with. Only the front and the home slot are read to find the entry: both hold
// valid entries or none (they are emptied together with the store).
void StateListPrefetch(const uint8_t* list, uint32_t nbytes, int stage) {
    const int mode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_STATELIST", 1))));
    if (mode != 1) return;
    SlFront& f = SlFrontOf(list);
    const SlSlot& home = g_slTable[SlHash(list) & (kSlSlots - 1)];
    if (stage == 1) {
        FePrefetch(&f);
        FePrefetch(&home);
        GuestWriteGenerationPrefetch(list, nbytes);
        return;
    }
    const SlEntry* e = f.list == list ? f.e : (home.list == list ? home.e : nullptr);
    if (!e) return;
    if (stage == 2) {
        FePrefetchLines(e, sizeof(SlEntry), 4);
        return;
    }
    if (!e->usable || e->nbytes != nbytes) return;
    if (stage == 3) {
        FePrefetchLines(e->ops.data(), static_cast<uint32_t>(e->ops.size() * sizeof(SlOp)), 8);
        FePrefetchLines(e->words.data(), static_cast<uint32_t>(e->words.size() * sizeof(uint32_t)), 6);
        return;
    }
    if (SlKeepBytes() && e->bytes.size() == nbytes) {
        const uint64_t gen = GuestWriteGeneration(list, nbytes);
        if (gen != e->gen && gen != kNotGuest) {
            FePrefetchLines(list, nbytes, 12);
            FePrefetchLines(e->bytes.data(), nbytes, 12);
        }
    }
}

// GXV_CACHE24 (vgx_state.h): what StateListFastHit of `list` reads, hinted in three stages, each from lines the stage before hinted a call
// ago: 1 the front slot (an address worked out from the list's address alone); 2 the entry's hot fields (the front holds the entry: it holds
// valid entries or none); 3 the compiled delta the replay walks (from the entry's hot fields: dp and dbytes). The list's generation counter
// and the table's home slot (a front miss) are left alone: the counters of a picture's lists share a few lines and the front hits.
void StateListAhead(const uint8_t* list, int stage) {
    const SlFront& f = SlFrontOf(list);
    if (stage == 1) { Pf24(&f); return; }
    if (f.list != list || !f.e) return;
    const SlEntry* e = f.e;
    if (stage == 2) { Pf24Lines(e, static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&e->quietUntil + 1) - reinterpret_cast<const uint8_t*>(e)), 2); return; }
    if (e->delta0 == 2 || e->delta0 == 4) Pf24Lines(e->dp, e->dbytes, Cache24DeltaLines());
}

void StateListCounters(uint64_t out[5]) { out[0] = g_slHits; out[1] = g_slBuilds; out[2] = g_slRefused; out[3] = g_slChecked; out[4] = g_slBad; }
void StateListFastCounters(uint64_t out[3]) { out[0] = g_slFastFront; out[1] = g_slFastTable; out[2] = g_slFastSame; }

// heap23b (GXV_SL_SCENEDROP, vgx_state.h): the store at a scene's destruction. Every entry is in the table exactly once (SlFind puts
// it in both; the clear at kSlMax empties both), so the table gives each entry's list address. Freed entries leave the store; the kept
// ones are put back into an emptied table (linear probing from their home slot, as SlFind places them) and the front is emptied
// (it is a cache in front of the table: a miss there looks the table up, StateListFastHit / StateListCall). Nothing outside this file
// holds an entry, and nothing here runs while a list is being replayed (the caller is the front end, between published bytes).
StateListStore StateListsSceneGone(const void* const* lo, const void* const* hi, int ranges, int mode) {
    StateListStore s;
    s.clears = g_slClears;
    auto bytesOf = [](const SlEntry& e) {
        uint64_t n = static_cast<uint64_t>(sizeof(SlEntry)) + e.bytes.capacity() + e.ops.capacity() * sizeof(SlOp) + e.words.capacity() * sizeof(uint32_t) +
                     e.delta.capacity() * sizeof(uint32_t);
        if (e.mix) { // (GXV_FEPLAN: a mixed list's plan and its sections)
            n += sizeof(MixPlan) + e.mix->steps.capacity() * sizeof(MixStep);
            for (const auto& x : e.mix->secs)
                n += sizeof(SlEntry) + x->ops.capacity() * sizeof(SlOp) + x->words.capacity() * sizeof(uint32_t) + x->delta.capacity() * sizeof(uint32_t);
        }
        return n;
    };
    std::vector<const SlEntry*> gone;
    for (const SlSlot& slot : g_slTable) {
        if (!slot.list || !slot.e) continue;
        bool drop = mode == 2;
        for (int i = 0; mode == 1 && !drop && i < ranges; ++i) {
            const uint8_t* const a = static_cast<const uint8_t*>(lo[i]);
            const uint8_t* const b = static_cast<const uint8_t*>(hi[i]);
            drop = a && b && a < b && slot.list >= a && slot.list < b;
        }
        const uint64_t n = bytesOf(*slot.e);
        if (drop) {
            ++s.dropped;
            s.droppedBytes += n;
            gone.push_back(slot.e);
        } else {
            ++s.entries;
            s.bytes += n;
        }
    }
    if (gone.empty()) return s;
    J3n2SceneGone(); // (GXV_J3DNATIVE=4 / 5: an active chain ends while the entries its tokens name are alive)
    ++g_slEpoch; // (GXV_J3DNATIVE: its records point at entries; they die with them)
    J3nSceneGone();
    for (SlFront& x : g_slFront) x = SlFront{};
    if (gone.size() == g_slStore.size()) {
        std::memset(g_slTable, 0, sizeof g_slTable);
        g_slStore.clear();
        return s;
    }
    std::sort(gone.begin(), gone.end());
    auto isGone = [&gone](const SlEntry* e) { return std::binary_search(gone.begin(), gone.end(), e); };
    // The table first (while every entry is alive): the kept slots, put back from their home slots in table order.
    std::vector<SlSlot> keep;
    keep.reserve(s.entries);
    for (const SlSlot& slot : g_slTable)
        if (slot.list && slot.e && !isGone(slot.e)) keep.push_back(slot);
    std::memset(g_slTable, 0, sizeof g_slTable);
    for (const SlSlot& k : keep) {
        uint32_t i = SlHash(k.list) & (kSlSlots - 1);
        while (g_slTable[i].list) i = (i + 1) & (kSlSlots - 1);
        g_slTable[i] = k;
    }
    // Then the store: the kept entries move to the front in order; the freed ones are deleted (by the move over them or the resize).
    size_t kept = 0;
    for (size_t i = 0; i < g_slStore.size(); ++i) {
        if (isGone(g_slStore[i].get())) continue;
        if (kept != i) g_slStore[kept] = std::move(g_slStore[i]);
        ++kept;
    }
    g_slStore.resize(kept);
    return s;
}

uint8_t StateListWhyLast(uint8_t* cmd) { if (cmd) *cmd = g_slBadCmd; return g_slWhy; }
namespace {
// (tinylist31: StateListCall's body; `plans` false: no mixed-list plan is made or run for this call, GXV_TINYLIST's tiny lists)
inline bool StateListCallP(const uint8_t* list, uint32_t nbytes, bool plans) {
    g_slWhy = 0;
    const int mode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_STATELIST", 1))));
    if (mode == 0) { g_slWhy = 1; return false; }
    const int bpFastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_BPFAST", 1))));
    const int bpFast = bpFastMode == 2 ? 1 : bpFastMode;
    // (GXV_FEPLAN: plans for mixed lists; not with the two-core front end or the J3D-native hooks)
#if defined(VGX_CLONE_B)
    const int fpMode = 0;
    (void)plans;
#else
    const int fpMode = (plans && FePlanMode() != 0 && vgx2_mode == 0 && J3nMode() == 0) ? FePlanMode() : 0;
#endif
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    if (gen == kNotGuest) { g_slWhy = 2; return false; }
    const uint64_t frame = CurrentFrame();
    SlFront& f = SlFrontOf(list);
    SlEntry* e;
    if (f.list == list && f.e) {
        e = f.e;
    } else {
        if (g_slStore.size() >= kSlMax) {
            for (SlFront& x : g_slFront) x = SlFront{};
            std::memset(g_slTable, 0, sizeof g_slTable);
            g_slStore.clear();
            ++g_slClears;
            J3n2SceneGone();
            ++g_slEpoch;
            J3nSceneGone();
        }
        e = SlFind(list);
        f.list = list;
        f.e = e;
    }
    if (frame < e->quietUntil) { g_slWhy = e->quietWhy ? e->quietWhy : 4; g_slBadCmd = e->quietCmd; return false; }
    bool valid = e->nbytes == nbytes && e->gen == gen && e->live;
    // A list whose memory was written since: the same bytes again (J3D writes its materials' lists every picture, mostly
    // with what they held) are told by comparing them with the copy kept at the parse (GXV_SLBYTES=1, default; a quarter of
    // a 64-bit hash's instructions, and exact), or by the hash (GXV_SLBYTES=0, as before).
    const bool keepBytes = SlKeepBytes();
    if (!valid && e->nbytes == nbytes && e->live &&
        (keepBytes ? (e->bytes.size() == nbytes && SameBytes(list, e->bytes.data(), nbytes)) : XXH3_64bits(list, nbytes) == e->hash)) {
        e->gen = gen;  // a write near it, not on it
        valid = true;
    }
    if (!valid) {
        if (e->live && e->nbytes == nbytes && ++e->changes >= 3) {
            // rewritten for most calls (animated material colours): parse as before for a few seconds
            e->changes = 0;
            e->quietUntil = frame + 300;
            e->quietWhy = 4;
            e->live = false;
            g_slWhy = 6;
            return false;
        }
        const uint64_t epoch0 = GuestWriteEpoch();
        e->nbytes = nbytes;
        e->usable = SlParse(*e, list, nbytes);
        e->mix.reset();
        // (GXV_FEPLAN: a list with draws / calls: its exact plan, made inside the same write-epoch window as the parse)
        if (!e->usable && fpMode != 0) MixBuild(*e, list, nbytes, frame);
        e->delta0 = 0; // (GXV_SLDELTA: compiled at its first replay in a later frame, so a list made again for every picture is not)
        e->parseFrame = frame;
        if (keepBytes) e->bytes.assign(list, list + nbytes);
        else e->hash = XXH3_64bits(list, nbytes);
        e->gen = gen;
        e->live = frame != 0;
        ++g_slBuilds;
        if (GuestWriteEpoch() != epoch0) {
            HostRange r{list, nbytes};
            if (GuestWritesTouch(epoch0, GuestWriteEpoch(), &r, 1)) { e->live = false; g_slWhy = 7; return false; }
        }
        if (!e->usable && !e->mix) {
            ++g_slRefused;
            e->quietUntil = frame + 600;
            e->quietWhy = 3;
            e->quietCmd = g_slBadCmd;
            g_slWhy = 5;
            return false;
        }
    } else {
        e->changes = 0;
        if (!e->usable && (!e->mix || fpMode == 0)) { g_slWhy = 5; g_slBadCmd = e->quietCmd; return false; }
        if (e->usable) ++g_slHits;
    }
    e->live = frame != 0;
#if defined(VGX_CLONE_B)
    if (SlHard(*e)) return false;
#endif
    if (e->mix) return MixCall(*e, list, nbytes, bpFast, fpMode); // (GXV_FEPLAN: a mixed list's plan)
    bool sideEffects = false;  // (a copy trigger, token or palette load must not run twice: such lists are not compared)
    if (diag::kEnabled && mode == 2)
        for (const SlOp& op : e->ops) sideEffects = sideEffects || (op.kind == 1 && kBpEffect.t[op.v >> 24]);
    if (diag::kEnabled && mode == 2 && !sideEffects) {
        // run both on the same starting state and compare what they leave
        static State before, byParser;  // (static: the state is large for a thread's stack)
        std::memcpy(&before, &g, sizeof(State));
        ProcessStream(list, nbytes);
        std::memcpy(&byParser, &g, sizeof(State));
        std::memcpy(&g, &before, sizeof(State));
        SlReplay(*e, bpFast);
        static uint64_t checked = 0, bad = 0;
        ++checked;
        ++g_slChecked;
        if (std::memcmp(&byParser, &g, sizeof(State)) != 0) {
            ++bad;
            ++g_slBad;
            VGX_LOG_N(16, "statelist: MISMATCH for list %p (%u bytes)\n", (const void*)list, nbytes);
            std::memcpy(&g, &byParser, sizeof(State));
        }
        if ((checked & 0x3FFFu) == 0) VGX_LOG_N(100, "statelist: %llu replays checked, %llu mismatching\n", (unsigned long long)checked, (unsigned long long)bad);
        return true;
    }
    SlRun(*e, bpFast);
    return true;
}
} // namespace
bool StateListCall(const uint8_t* list, uint32_t nbytes) { return StateListCallP(list, nbytes, true); }
#include "vgx_tinylist.inc" // GXV_TINYLIST, GXV_TINYLIST_CENSUS (tinylist31)

#include "vgx_j3native.inc" // GXV_J3DNATIVE
#include "vgx_j3native2.inc" // GXV_J3DNATIVE=4 / 5 (stage 2)

// ---- GXV_FE_TWOCORE (vgx_twocore.h): this file's per-stream state for the hand-overs, and the second worker's cold start
extern "C" uint64_t VGX2_SIDE(regs_hash)(void) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
    uint64_t r = 0;
    auto part = [&](int k) { r |= (h & 0xFFull) << (8 * k); h = 1469598103934665603ull; };
    mix(g.bp, sizeof g.bp); part(0);
    mix(g.xfPos, sizeof g.xfPos); part(1);
    mix(g.xfNrm, sizeof g.xfNrm); mix(g.xfPost, sizeof g.xfPost); part(2);
    mix(g.xfLight, sizeof g.xfLight); part(3);
    mix(g.xfReg, sizeof g.xfReg); part(4);
    mix(g.vcd, sizeof g.vcd); mix(g.vat, sizeof g.vat); part(5);
    mix(g.cpBase, sizeof g.cpBase); mix(g.cpStride, sizeof g.cpStride); part(6);
    mix(&g.matIdxA, 8); mix(&g.bpMask, 4); part(7);
    return r;
}
// (twocore25: the register file without its palette slots, as two pieces: before and after State::tlut. The slots are 32 KiB of the 39.5 KiB;
// the player never took B's (Adopt kept its own, copying them aside and back: three 32 KiB copies at every segment taken, the hand-over's
// main cost), and the verify compares no slot. vgx2_state_split gives the pieces' sizes for the player's checks.)
extern "C" int VGX2_SIDE(state_objs)(Vgx2Obj* out) {
    constexpr size_t a = offsetof(State, tlut), b = offsetof(State, tlut) + sizeof(State::tlut);
    out[0] = Vgx2Obj{&g, static_cast<uint32_t>(a)};
    out[1] = Vgx2Obj{reinterpret_cast<uint8_t*>(&g) + b, static_cast<uint32_t>(sizeof(State) - b)};
    return 2;
}
#if defined(VGX_CLONE_B)
// (the second worker's scan: state commands parsed in lists so far, to tell a list of draws only)
extern "C" uint64_t vgx2_b_list_state_cmds(void) { return g_sd.stateCmds[1]; }
// Every dirty flag up: the segment's first resolution builds and sends everything (vgx_twocore.cpp).
extern "C" void vgx2_b_state_cold(void) {
    g.bpDirty = g.bpKeyDirty = g.bpRsDirty = g.xfRegDirty = g.xfKeyDirty = g.xfViewDirty = g.xfProjDirty = g.xfColorDirty = g.xfMtxDirty = g.xfLightDirty = true;
    g.texDirty = true;
    g.xfPosRowsDirty = ~0ull;
    g.tevColDirty = 0xFF;
    g.texSlotDirty = 0xFF;
    g.bpTexStale = 0xFF;
}
#endif
} // namespace vgx
#if !defined(VGX_CLONE_B)
extern "C" void port_vgx_verify_summary(void) {
    char line[420];
    if (vgx::VerifySummaryLine(line, sizeof line)) port_log_summary("verify: %s(cumulative)", line);
}
#endif

// keys27 (GXV_KEYS27): a draw in play must never make the picture wait for a shader compile or a program link.
// Pure (vgx_shadergen.h's key structs and the standard library only; the front end, the back end and tests/keys27_model.cpp share it).
//
// Why a draw in play still compiled (c86 tour, Beach Bowl arrival: six demand compiles, 1,739 ms): several fragment key bits are chosen PER DRAW from values or from the
// render state, not from the material, so a list made from materials can never hold them, and every one of them is a SIBLING of a program that is resident:
//   inWrap / preWrap / dualWrap  a TEV register outside 0..255 (State::tevOor, BuildFsKey)             value driven: 2 shapes (none / wrapped); the wrap masks follow from the key
//   regFromVtx = 3               register folding (FoldRegs): a small CPU-transformed draw           value driven: the draw's vertex count and FoldValuesOk(); 2 shapes
//   rejectZero                   an alpha-tested draw whose blend makes a zero source a no-op        render-state driven (RejectIsNoop): 2 shapes
//   alphaDead                    a draw whose alpha output nothing uses                              render-state driven (AlphaOutputUnused): 2 shapes
//   atPass                       a draw whose alpha test passes for every fragment                   value driven (FsValueEval: texture alpha ranges, registers): 2 shapes
//   dstAlpha                     GXSetDstAlpha constant                                              render-state driven: 2 shapes
// So one material can reach up to 2 x 2 x 3 (none / rejectZero / alphaDead / atPass, + atPass of a tested key) fragment shaders.
//
// The remedy is the GENERAL form: the key with the optimisation bits cleared. For the three render-state / value variants whose only difference is the alpha path (rejectZero,
// alphaDead, atPass) the general key's text reads the same u_tev block, the same samplers and the same varyings and computes the same result for the draw (it IS what the front end
// started from: RejectZeroFs / AlphaDeadFs / AtPassFs derive the variant from the general id), so the back end may draw with the general program whenever the specialised one is not
// resident, and compile the specialised one later. The register-folding sibling is NOT back-end substitutable (a folded draw carries registers 1 and 2 in its vertices and left them out
// of the u_tev block): the front end must choose (Resident) before it folds. The wrap siblings change what the block holds (preWrap rows): same.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vgx_shadergen.h"

namespace vgx::keys27 {

constexpr uint32_t kMaxIds = 8192; // fragment shader ids tracked (a tour makes ~2,400)
enum Kind : uint8_t { kNone = 0, kRejectZero = 1, kAlphaDead = 2, kAtPass = 3, kColorDead = 4 }; // the exact back-end siblings
// Value-driven bits of a key (ValueMask).
enum : uint8_t { kVmWrap = 1, kVmPreWrap = 2, kVmDualWrap = 4, kVmFold = 8, kVmRejectZero = 16, kVmAtPass = 32, kVmAlphaDead = 64, kVmDstAlpha = 128 };

struct Link {
    uint16_t general = 0; // the general form's id + 1 (0: this key is its own general form)
    uint8_t kind = 0;     // Kind
    uint8_t mask = 0;     // ValueMask of this id's key
    uint32_t shape = 0;   // Shape of this id's key (the key without its value-driven bits)
};
// Written by the front end when it makes a variant (before the id reaches a draw record: the command queue orders it), read by the back end. Plain loads and stores of aligned words.
inline Link g_link[kMaxIds];

inline uint8_t ValueMask(const FsKey& k) {
    uint8_t m = 0;
    if (k.inWrap & 1) m |= kVmWrap;
    if (k.preWrap()) m |= kVmPreWrap;
    if (k.dualWrap()) m |= kVmDualWrap;
    if (k.regFromVtx) m |= kVmFold;
    if (k.rejectZero()) m |= kVmRejectZero;
    if (k.atPass()) m |= kVmAtPass;
    if (k.alphaDead()) m |= kVmAlphaDead;
    if (k.dstAlpha) m |= kVmDstAlpha;
    return m;
}
// The key as it is without any value-driven bit: two keys of one shape are siblings.
inline FsKey Canonical(const FsKey& k) {
    FsKey c = k;
    c.inWrap = 0;
    c.regFromVtx = 0;
    c.dstAlpha = 0;
    c.setPreWrap(0);
    c.setDualWrap(0);
    c.setRejectZero(0);
    c.setAtPass(0);
    c.setAlphaDead(0);
    c.setColorDead(0);
    return c;
}
inline uint32_t Fnv(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}
inline uint32_t Shape(const FsKey& k) { const FsKey c = Canonical(k); return Fnv(&c, sizeof c); }

// The exact general form of a key: rejectZero, alphaDead and atPass cleared. Returns false when there is nothing to clear.
inline bool ExactGeneral(const FsKey& k, FsKey& out) {
    if (!k.rejectZero() && !k.alphaDead() && !k.atPass() && !k.colorDead()) return false;
    out = k;
    out.setRejectZero(0);
    out.setAlphaDead(0);
    out.setAtPass(0);
    out.setColorDead(0);
    return true;
}
inline void SetGeneral(uint32_t id, uint32_t generalId, Kind kind, const FsKey& key) {
    if (id >= kMaxIds) return;
    Link& l = g_link[id];
    l.general = static_cast<uint16_t>(generalId < kMaxIds ? generalId + 1 : 0);
    l.kind = kind;
    l.mask = ValueMask(key);
    l.shape = Shape(key);
}
// GXV_KEYS27_DIFF: the back end registers its receiver (a static initialiser); the front end calls Hook for every key it makes (fragment 1 / vertex 0) when the switch is on, so that the back end can print
// a demand key's difference from the nearest resident one. The environment is read at the first key (after main has applied the env file).
inline void (*g_keyHookImpl)(int fragment, uint32_t id, const void* key, uint32_t size) = nullptr;
inline bool DiffWanted() { static const bool on = [] { const char* e = std::getenv("GXV_KEYS27_DIFF"); return e && *e && *e != '0'; }(); return on; }
inline void Hook(int fragment, uint32_t id, const void* key, uint32_t size) { if (g_keyHookImpl && DiffWanted()) g_keyHookImpl(fragment, id, key, size); }
inline void NoteKey(uint32_t id, const FsKey& key) { // every new key (the forensics: shape and value bits)
    Hook(1, id, &key, static_cast<uint32_t>(sizeof key));
    if (id >= kMaxIds) return;
    g_link[id].mask = ValueMask(key);
    g_link[id].shape = Shape(key);
}
// The chain of exact general forms of id (at most 3 steps), as ids; returns the count. out[0] is the nearest.
inline int GeneralChain(uint32_t id, uint16_t out[3]) {
    int n = 0;
    while (n < 3 && id < kMaxIds && g_link[id].general) {
        const uint32_t g = static_cast<uint32_t>(g_link[id].general) - 1u;
        if (g == id) break;
        out[n++] = static_cast<uint16_t>(g);
        id = g;
    }
    return n;
}

// ---- field-by-field difference of two keys (forensics: "keys27-diff:" lines). Returns the number of differing fields; the text lists them (truncated to n).
struct Out {
    char* p; size_t n, len; unsigned fields;
    Out(char* buf, size_t cap) : p(buf), n(cap), len(0), fields(0) { if (n) p[0] = 0; }
    void Add(const char* name, unsigned a, unsigned b) {
        ++fields;
        if (len + 1 >= n) return;
        const int w = snprintf(p + len, n - len, "%s%s %x>%x", len ? "; " : "", name, a, b);
        if (w > 0) len = len + static_cast<size_t>(w) < n ? len + static_cast<size_t>(w) : n - 1;
    }
};
#define K27_F(f) do { if (a.f != b.f) o.Add(#f, a.f, b.f); } while (0)
inline unsigned FsDiff(const FsKey& a, const FsKey& b, char* text, size_t cap) {
    Out o(text, cap);
    K27_F(numStages); K27_F(numTexGens); K27_F(texProjMask); K27_F(alphaTestOn); K27_F(alphaTest); K27_F(fog); K27_F(dstAlpha); K27_F(numIndStages); K27_F(ztex);
    K27_F(regFromVtx); K27_F(logicSrc); K27_F(inWrap);
    if (a.texVec2() != b.texVec2()) o.Add("texVec2", a.texVec2(), b.texVec2());
    if (a.rejectZero() != b.rejectZero()) o.Add("rejectZero", a.rejectZero(), b.rejectZero());
    if (a.preWrap() != b.preWrap()) o.Add("preWrap", a.preWrap(), b.preWrap());
    if (a.dualWrap() != b.dualWrap()) o.Add("dualWrap", a.dualWrap(), b.dualWrap());
    if (a.alphaDead() != b.alphaDead()) o.Add("alphaDead", a.alphaDead(), b.alphaDead());
    if (a.colorDead() != b.colorDead()) o.Add("colorDead", a.colorDead(), b.colorDead());
    if (a.atPass() != b.atPass()) o.Add("atPass", a.atPass(), b.atPass());
    if (a.preAlpha() != b.preAlpha()) o.Add("preAlpha", a.preAlpha(), b.preAlpha());
    if (a.silhFuse() != b.silhFuse()) o.Add("silhFuse", a.silhFuse(), b.silhFuse());
    if (a.indPre() != b.indPre()) o.Add("indPre", a.indPre(), b.indPre());
    const int ns = a.numStages > b.numStages ? a.numStages : b.numStages;
    for (int s = 0; s < ns && s < 16; ++s) {
        const FsKey::Stage &x = a.stage[s], &y = b.stage[s];
        char nm[24];
        if (x.color != y.color) { snprintf(nm, sizeof nm, "st%d.color", s); o.Add(nm, x.color, y.color); }
        if (x.alpha != y.alpha) { snprintf(nm, sizeof nm, "st%d.alpha", s); o.Add(nm, x.alpha, y.alpha); }
        if (x.ind != y.ind) { snprintf(nm, sizeof nm, "st%d.ind", s); o.Add(nm, x.ind, y.ind); }
        if (x.texEnable != y.texEnable || x.texMap != y.texMap || x.texCoord != y.texCoord || x.rasChan != y.rasChan) {
            snprintf(nm, sizeof nm, "st%d.tex/ras", s); o.Add(nm, (x.texEnable << 12) | (x.texMap << 8) | (x.texCoord << 4) | x.rasChan, (y.texEnable << 12) | (y.texMap << 8) | (y.texCoord << 4) | y.rasChan);
        }
        if (x.kcsel != y.kcsel || x.kasel != y.kasel) { snprintf(nm, sizeof nm, "st%d.konst", s); o.Add(nm, (x.kcsel << 8) | x.kasel, (y.kcsel << 8) | y.kasel); }
        if (x.rasSwap != y.rasSwap || x.texSwap != y.texSwap) { snprintf(nm, sizeof nm, "st%d.swap", s); o.Add(nm, (x.rasSwap << 8) | x.texSwap, (y.rasSwap << 8) | y.texSwap); }
        if (x.texOff != y.texOff || x.lodBias != y.lodBias) { snprintf(nm, sizeof nm, "st%d.off/lod", s); o.Add(nm, (x.texOff << 8) | x.lodBias, (y.texOff << 8) | y.lodBias); }
    }
    for (int i = 0; i < 4; ++i) {
        if (a.ind[i].texMap != b.ind[i].texMap || a.ind[i].texCoord != b.ind[i].texCoord || a.ind[i].pad[0] != b.ind[i].pad[0]) {
            char nm[16]; snprintf(nm, sizeof nm, "ind%d", i);
            o.Add(nm, (a.ind[i].texMap << 8) | (a.ind[i].texCoord << 4) | a.ind[i].pad[0], (b.ind[i].texMap << 8) | (b.ind[i].texCoord << 4) | b.ind[i].pad[0]);
        }
    }
    return o.fields;
}
#undef K27_F
inline unsigned VsDiff(const VsKey& a, const VsKey& b, char* text, size_t cap) {
    Out o(text, cap);
    if (a.numTexGens != b.numTexGens) o.Add("numTexGens", a.numTexGens, b.numTexGens);
    if (a.numChans != b.numChans) o.Add("numChans", a.numChans, b.numChans);
    if (a.perVtxPos != b.perVtxPos) o.Add("perVtxPos", a.perVtxPos, b.perVtxPos);
    if (a.fogVtx != b.fogVtx) o.Add("fogVtx", a.fogVtx, b.fogVtx);
    for (int i = 0; i < 4; ++i) {
        if (std::memcmp(&a.chan[i], &b.chan[i], sizeof a.chan[i]) != 0) { char nm[12]; snprintf(nm, sizeof nm, "chan%d", i); o.Add(nm, a.chan[i].enable | (a.chan[i].lightMask << 8), b.chan[i].enable | (b.chan[i].lightMask << 8)); }
    }
    for (int i = 0; i < 8; ++i) {
        if (std::memcmp(&a.tg[i], &b.tg[i], sizeof a.tg[i]) != 0) { char nm[12]; snprintf(nm, sizeof nm, "tg%d", i); o.Add(nm, a.tg[i].type | (a.tg[i].src << 8), b.tg[i].type | (b.tg[i].src << 8)); }
    }
    return o.fields;
}

// ---- the census (one "keys27:" line every 10 s and at a stage's end; the back end fills it)
struct Census {
    uint64_t demandInPlay = 0;   // programs a draw made (compile and/or link) on the render thread in play: must be 0
    double demandMs = 0, demandWorstMs = 0;
    uint64_t fallbackGeneral = 0, fallbackOther = 0; // draws drawn with the general form / with another exactly-correct resident program, while the specialised one was missing
    uint64_t blocked = 0;        // protected draws with no exact resident program (compiled / linked at once: "keys27: BLOCKED")
    uint64_t skipped = 0;        // soft draws left out for a picture or more (existing async path)
    uint64_t wants = 0, wantsLinked = 0, wantsAsked = 0, wantsDropped = 0; // specialised siblings asked for / linked later / handed to the worker / refused by the caps
    uint64_t learned = 0;        // programs added to the stage's learned list because a draw wanted them
    uint64_t workerCompiles = 0; double workerMs = 0; // worker jobs done (a snapshot of the engine's counters)
    unsigned workerQueue = 0; bool workerOn = false;
    uint64_t diffLines = 0;
};
inline int FormatCensus(char* out, size_t n, const char* stage, const Census& c) {
    return snprintf(out, n,
                    "keys27: %s: demand compiles in play %u (%.0f ms, worst %.0f ms) [must be 0]; fallbacks general %u, other-exact %u; blocked %u; skipped %u; siblings wanted %u (linked later %u, to worker %u, refused %u); "
                    "learned %u; worker %s: queue %u, %u jobs %.0f ms",
                    stage ? stage : "-", static_cast<unsigned>(c.demandInPlay), c.demandMs, c.demandWorstMs, static_cast<unsigned>(c.fallbackGeneral), static_cast<unsigned>(c.fallbackOther),
                    static_cast<unsigned>(c.blocked), static_cast<unsigned>(c.skipped), static_cast<unsigned>(c.wants), static_cast<unsigned>(c.wantsLinked), static_cast<unsigned>(c.wantsAsked),
                    static_cast<unsigned>(c.wantsDropped), static_cast<unsigned>(c.learned), c.workerOn ? "ON" : "OFF", c.workerQueue, static_cast<unsigned>(c.workerCompiles), c.workerMs);
}

// ---- the expansion of a farm list (GXV_KEYS27_EXPAND): the sibling keys a draw can reach from a key that was recorded. fold needs the vertex key (foldOk = FoldKeysOk); the wrap siblings need FsTevInPreWrapMask and are made by the front end (vgx_draw.cpp). Returns the number of keys written to out (at most max); none equals the input.
inline int Siblings(const FsKey& k, bool foldOk, FsKey* out, int max) {
    int n = 0;
    auto add = [&](const FsKey& v) { if (n < max && std::memcmp(&v, &k, sizeof v) != 0) out[n++] = v; };
    const bool tested = k.alphaTestOn && !k.atPass();
    if (!k.rejectZero() && !k.atPass() && !k.alphaDead() && !k.silhFuse() && !k.preAlpha()) {
        if (tested) { FsKey v = k; v.setRejectZero(1); add(v); FsKey w = k; w.setAtPass(1); add(w); }
        else if (!k.alphaTestOn && !k.dstAlpha) { FsKey v = k; v.setAlphaDead(1); add(v); }
    }
    const int base = n;
    auto mkFold = [](FsKey v) {
        v.regFromVtx = 3;
        // c0/c1 come from vertex colours (in 0..1), never from u_tev preWrap/dualWrap rows (matches CanonListFsKey).
        v.setPreWrap(static_cast<uint8_t>(v.preWrap() & ~0x66u));
        v.setDualWrap(static_cast<uint8_t>(v.dualWrap() & ~0x06u));
        return v;
    };
    if (foldOk && !k.regFromVtx) { add(mkFold(k)); for (int i = 0; i < base && n < max; ++i) add(mkFold(out[i])); }
    return n;
}

} // namespace vgx::keys27

// paljoin31 (plan-gpu31 A-T1): GXV_PALJOIN_CENSUS (default 0, read once; LOG ONLY: nothing is drawn, merged or changed differently).
//
// The question: how many of a picture's skinned static draws could be ONE draw if the matrix groups of a shape shared one palette per
// model? J3D draws a skinned shape as matrix groups: J3DShapeMtxMulti::load() writes the group's matrices into position matrix memory
// slots 0, 3, 6, ... (J3DFifoLoadPosMtxImm(m, slot * 3)) and the group's display list names a slot in every vertex (PNMTXIDX); the next
// group loads other matrices into the same slots. The vertex shader reads u_pal[pi .. pi+2] with pi = a_pos.w (vgx_shadergen.cpp, the
// perVtxPos branch). Joining two groups is an INDEX REMAP: give every distinct matrix of the model one palette entry, rename the slots in
// the vertex data, draw both groups at once. The matrices, the expression and the triangle order stay the same, so the result is exact.
//
// A run of consecutive palette draws can be joined when (all of them, exactly):
//   * the same model: the same position and normal array base (a J3D model data has one array of each; its shapes share them);
//   * the same state: the same render-state serial, the same fragment shader, the same vertex shader apart from the palette class
//     (the class is chosen again from the joined palette), and no register written between them other than matrix memory
//     (the register file's dirty flags, matrix memory excluded);
//   * nothing sent between them (no other draw, no copy, no state command that made a job): the job counter has not moved;
//   * no per-vertex texture matrix (a palette texgen names rows the census does not see: counted, never joined);
//   * the union of the matrices (by content: position rows and normal rows) is no larger than the palette the shader can hold.
//     The shader's u_pal has 96 vec4: position rows 0..63 (the index clamps at 61: 21 matrices of 3 rows) and normal rows 64..93 (the
//     normal index is 64 + pi up to 29, then 93: 10 matrices). A shape that reads normals is held to 10, one that does not to 21. A bigger
//     palette needs a uniform buffer (plan-gpu31 A.4), so cap 2 counts the joins with no size limit at all.
// "Removed" draws: a chain of k joinable draws becomes one, so k - 1 draws go. Of those, the ones whose palette differs from the draw
// before (a matrix reload between them) are the real saving ("reload"); one whose palette equals the one before is not a draw the join
// creates (the record stage can already merge it) and is counted apart ("same").
//
// This header is the whole decision, with no dependence on the renderer, so a host test can drive it with a synthetic model
// (tests/paljoin_census.cpp). vgx_draw.cpp builds one Draw per static palette run (EmitStaticRun) and times the run's front end.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace paljoin {

constexpr int kCaps = 3;                          // palette sizes counted side by side (matrices): the shader's lit / unlit ceilings, then none
constexpr int kLitMtx = 10, kUnlitMtx = 21;
constexpr int kMaxSlots = 22;                     // matrices one draw can name (position rows 0..63)
constexpr int kChainMats = 256;

#ifndef PALJOIN_MUTANT
#define PALJOIN_MUTANT 0 // (tests only: tools/test_paljoin_census.sh breaks one rule at a time and expects the test to fail)
#endif

// Why a palette draw did not continue the chain before it (the shader's own ceiling, lit).
enum Break : uint8_t { BkJoined, BkFirst, BkModel, BkSent, BkState, BkRegs, BkCap, BkIneligible, BkCount };
inline const char* BreakName(int b) {
    static const char* const n[BkCount] = {"joined", "first", "other model", "drawn between", "state differs", "registers written", "palette full", "not joinable"};
    return n[b >= 0 && b < BkCount ? b : 0];
}
enum Inel : uint8_t { InOk, InRowsUnknown, InRowAbove61, InTexgenPal, InCount };
inline const char* InelName(int b) {
    static const char* const n[InCount] = {"ok", "rows unknown", "row above 61", "per-vertex texture matrix"};
    return n[b >= 0 && b < InCount ? b : 0];
}

struct Draw {
    uint64_t model = 0;        // position array base << 32 | normal array base (0: not known: such a draw joins nothing)
    uint32_t serial = 0;       // the render state serial (cur.stateSerial)
    uint32_t useFs = 0;        // the fragment shader
    uint64_t vsNorm = 0;       // hash of the vertex key with the palette class set to one value
    uint32_t jobSeq = 0;       // the job counter when the draw was made, before its own job
    bool regsWritten = false;  // a register other than matrix memory was written since the last drawn run
    bool lit = true;           // the shape reads normals (the palette's normal rows then hold it to 10 matrices)
    uint8_t palClass = 0;      // perVtxPos: 1 full (96 vec4), 2 short (60), 3 small (24)
    uint8_t ineligible = InOk;
    uint8_t nMat = 0;          // distinct matrices (by content) the draw's vertices name
    uint64_t mat[kMaxSlots] = {};
    uint64_t image = 0;        // what the palette holds in the slots the draw uses (slot, content), in slot order
    uint32_t vertices = 0, indices = 0;
};

struct Verdict {
    bool joined[kCaps] = {};   // continues the chain before it (the draw would not be a draw of its own)
    bool reload = false;       // ... and its palette differs from the draw before it (cap 0)
    uint8_t brk = BkFirst;     // cap 0: why not
    uint32_t modelSlot = 0;
};

struct ModelRow { uint64_t model = 0, draws = 0, removed = 0, ns = 0, removedNs = 0; };

struct Counters {
    uint64_t pictures = 0, staticRuns = 0, palDraws = 0, palNs = 0, palVerts = 0, palIdx = 0;
    uint64_t byClass[4] = {}; // 1 full, 2 short, 3 small
    uint64_t ineligible[InCount] = {};
    uint64_t brk[BkCount] = {};
    uint64_t removed[kCaps] = {}, removedReload[kCaps] = {}, removedSame[kCaps] = {};
    uint64_t removedNs[kCaps] = {}, removedReloadNs[kCaps] = {};
    uint64_t removedVerts = 0;
    uint64_t chains[kCaps] = {};
    uint64_t chainLen[kCaps][5] = {}; // chains of 1, 2, 3-4, 5-8, 9+ draws
    uint64_t mtxSum = 0, mtxMax = 0;  // matrices a palette draw names
    uint64_t palUploadBytes = 0;      // 384 / 960 / 1536 a draw: an upper bound (a draw whose uniforms did not change uploads nothing)
};

class Census {
public:
    static constexpr int kModels = 64;
    Counters c;
    ModelRow models[kModels];
    int nModels = 0;
    uint64_t modelsLost = 0;

    void Reset() { *this = Census(); }

    // One palette static run, before it is sent. AddNs() then adds the run's front-end time to its verdict; Sent() tells the job counter.
    Verdict Note(const Draw& d) {
        Verdict v;
        ++c.palDraws;
        c.palVerts += d.vertices;
        c.palIdx += d.indices;
        if (d.palClass >= 1 && d.palClass <= 3) {
            ++c.byClass[d.palClass];
            c.palUploadBytes += d.palClass == 3 ? 384u : d.palClass == 2 ? 960u : 1536u;
        }
        c.mtxSum += d.nMat;
        c.mtxMax = std::max<uint64_t>(c.mtxMax, d.nMat);
        v.modelSlot = ModelSlot(d.model);
        if (v.modelSlot < kModels) ++models[v.modelSlot].draws;
        const bool ok = d.ineligible == InOk && d.model != 0 && d.nMat != 0;
        if (!ok) ++c.ineligible[d.ineligible != InOk ? d.ineligible : InRowsUnknown];
        for (int k = 0; k < kCaps; ++k) {
            Chain& ch = chain_[k];
            uint8_t why = BkJoined;
            if (!ok) why = BkIneligible;
            else if (!ch.valid) why = BkFirst;
            else if (ch.model != d.model && PALJOIN_MUTANT != 4) why = BkModel;
            else if (ch.jobSeq != d.jobSeq && PALJOIN_MUTANT != 3) why = BkSent;
            else if ((ch.serial != d.serial || ch.useFs != d.useFs || ch.vsNorm != d.vsNorm) && PALJOIN_MUTANT != 1) why = BkState;
            else if (d.regsWritten && PALJOIN_MUTANT != 2) why = BkRegs;
            if (why == BkJoined) {
                int cap = k == 0 ? (d.lit ? kLitMtx : kUnlitMtx) : k == 1 ? kUnlitMtx : (1 << 20);
                if (k == 0 && d.lit && PALJOIN_MUTANT == 5) cap += 1;
                if (k == 0 && PALJOIN_MUTANT == 6) cap = 1 << 20;
                const int u = UnionSize(ch, d);
                if (u > cap || u > kChainMats) why = BkCap;
            }
            if (k == 0) {
                ++c.brk[why];
                v.brk = why;
            }
            if (why == BkJoined) {
                v.joined[k] = true;
                const bool reload = d.image != ch.image || PALJOIN_MUTANT == 7;
                ++c.removed[k];
                if (reload) ++c.removedReload[k]; else ++c.removedSame[k];
                if (k == 0) {
                    v.reload = reload;
                    c.removedVerts += d.vertices;
                    if (v.modelSlot < kModels) ++models[v.modelSlot].removed;
                }
                ++ch.len;
                Absorb(ch, d);
            } else {
                CloseChain(k);
                if (ok) Start(ch, d);
            }
        }
        return v;
    }
    // The job counter after the run was sent: the next run joins only if nothing moved it further.
    void Sent(uint32_t jobSeqAfter) {
        for (int k = 0; k < kCaps; ++k)
            if (chain_[k].valid) chain_[k].jobSeq = jobSeqAfter;
    }
    void AddNs(const Verdict& v, uint64_t ns) {
        c.palNs += ns;
        if (v.modelSlot < kModels) models[v.modelSlot].ns += ns;
        for (int k = 0; k < kCaps; ++k)
            if (v.joined[k]) {
                c.removedNs[k] += ns;
                if (k == 0 && v.reload) c.removedReloadNs[0] += ns;
                if (k == 0 && v.modelSlot < kModels) models[v.modelSlot].removedNs += ns;
            }
    }
    void StaticRun() { ++c.staticRuns; }
    // A picture ended (the job counter restarts): no chain goes across it.
    void PictureEnd() {
        Flush();
        ++c.pictures;
    }
    void Flush() {
        for (int k = 0; k < kCaps; ++k) { CloseChain(k); chain_[k] = Chain(); }
    }

private:
    struct Chain {
        bool valid = false;
        uint64_t model = 0, vsNorm = 0, image = 0;
        uint32_t serial = 0, useFs = 0, jobSeq = 0, len = 0;
        int n = 0;
        uint64_t mats[kChainMats] = {};
    };
    Chain chain_[kCaps];

    static int UnionSize(const Chain& ch, const Draw& d) {
        int n = ch.n;
        for (int i = 0; i < d.nMat; ++i) {
            bool have = false;
            for (int j = 0; j < ch.n && !have; ++j) have = ch.mats[j] == d.mat[i];
            if (!have) ++n;
        }
        return n;
    }
    static void Absorb(Chain& ch, const Draw& d) {
        for (int i = 0; i < d.nMat; ++i) {
            bool have = false;
            for (int j = 0; j < ch.n && !have; ++j) have = ch.mats[j] == d.mat[i];
            if (!have && ch.n < kChainMats) ch.mats[ch.n++] = d.mat[i];
        }
        ch.image = d.image;
    }
    static void Start(Chain& ch, const Draw& d) {
        ch = Chain();
        ch.valid = true;
        ch.model = d.model; ch.serial = d.serial; ch.useFs = d.useFs; ch.vsNorm = d.vsNorm; ch.jobSeq = d.jobSeq;
        ch.len = 1;
        Absorb(ch, d);
    }
    void CloseChain(int k) {
        Chain& ch = chain_[k];
        if (!ch.valid) return;
        ++c.chains[k];
        ++c.chainLen[k][ch.len <= 1 ? 0 : ch.len == 2 ? 1 : ch.len <= 4 ? 2 : ch.len <= 8 ? 3 : 4];
        ch.valid = false;
    }
    uint32_t ModelSlot(uint64_t m) {
        if (m == 0) return kModels; // (not counted by model)
        for (int i = 0; i < nModels; ++i)
            if (models[i].model == m) return static_cast<uint32_t>(i);
        if (nModels >= kModels) { ++modelsLost; return kModels; }
        models[nModels].model = m;
        return static_cast<uint32_t>(nModels++);
    }
};

// FNV-1a over 32-bit words: the content hash of a matrix, the vertex key, the palette image.
inline uint64_t HashWords(const uint32_t* w, int n, uint64_t h = 1469598103934665603ull) {
    for (int i = 0; i < n; ++i) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}

inline uint64_t HashBytes(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace paljoin

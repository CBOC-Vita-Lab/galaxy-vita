// GXV_HSR_ORDER (hsrorder25): within one scene, opaque draws first, then alpha-tested ones, where that cannot change the picture.
//
// Why: the PowerVR SGX543's hidden-surface removal makes hidden OPAQUE fragments free only while opaque geometry is submitted together;
// an alpha-tested (`discard`) or blended draw in the middle of a run of opaque draws makes the hardware resolve what is visible so far and
// shade it, pass after pass. The Wii's GPU had no HSR, so the game's own draw order never cared. The planner below is a pure function of
// a picture's command list (no GL, no globals but its scratch): it finds the SEGMENTS in which draws may be exchanged and, for GXV_HSR_ORDER=1,
// returns the command list with each segment stably partitioned (opaque draws in their order, then alpha-tested draws in their order).
// GXV_HSR_ORDER=2 only counts (census), 3 alternates original and reordered pictures (a verify aid).
//
// What a segment is (everything else is a BARRIER: it stays where it is and nothing is moved across it):
//   * every command that is not a draw (copies, pass begin, uploads, shader / program / variant commands, present, target change);
//   * every draw that is not "plain" (see Classify): blended (always), no depth test, no depth write, a depth function other than LESS /
//     LEQUAL, a colour or alpha mask, depth-only draws, a covering quad (the game's clears and fills), a stencil operation (shadow window),
//     a HUD-marked draw, a draw inside an offscreen pass, a line / point draw, an empty draw;
//   * an alpha-tested draw that a LATER opaque draw of the segment could tie with in depth (GXV_HSR_ORDER_TIES, see Plan).
// Exactness: two plain draws (depth test + write with LESS or LEQUAL, colour and alpha overwritten, no blend, no stencil, no framebuffer
// read) commute except for fragments of equal depth: the colour and depth at a pixel are those of the nearest surviving fragment (a killed
// fragment writes nothing, a surviving one writes colour and depth, a fragment passes iff z <= (<) the depth then there; the depth only ever
// falls, so the final depth is the minimum over the surviving fragments and the colour is that fragment's). Only when two surviving fragments
// share the exact minimum depth does the order decide (LEQUAL: the later draw wins; LESS: the earlier one). Opaque-opaque and alpha-alpha pairs
// keep their order, so only the pairs (alpha-tested draw A before opaque draw B in the original) change, and only at pixels where A and B
// both survive with the same minimal depth: coplanar geometry drawn through the same transform. GXV_HSR_ORDER_TIES keeps those pairs in order.
// (ties25) GXV_HSR_ORDER_TIES=4 pins such a pair only when the two draws CAN tie: see TieOracle below and vgx_hsrgeo.h.
#pragma once
#include "vgx_cmd.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
namespace hsr {

// Why a draw or command stays where it is (the census's reasons).
enum Reason : uint8_t {
    kRNone = 0,    // movable
    kRCmd,         // not a draw (the command's own type is counted in FrameStat::cmdType)
    kRBlend,       // blended
    kRDepthOnly,   // writes neither colour nor alpha
    kRNoTest,      // no depth test
    kRNoWrite,     // depth test but no depth write
    kRFunc,        // depth function other than LESS / LEQUAL
    kRMask,        // colour xor alpha written
    kRCover,       // a covering quad (clear / fill)
    kRStencil,     // a stencil operation (GXV_SHADOW_STENCIL window)
    kRHud,         // HUD-marked
    kRPass,        // inside an offscreen pass
    kRPrim,        // lines / points
    kREmpty,       // no indices
    kRPinXf,       // alpha-tested, a later opaque draw of the segment has the same transform
    kRPinModel,    // alpha-tested, a later opaque draw of the segment has the same static buffer
    kRNoLater,     // alpha-tested and movable, but no opaque draw follows it in its segment
    kRUnknown,     // the fragment shader is not registered yet (its CMD_SHADER comes later in the buffer): class not known
    // (ties25, GXV_HSR_ORDER_TIES=4) alpha-tested, a later opaque draw of the same transform that CAN tie with it, by what the proof found:
    kRPinCoplanar, // coplanar overlapping triangles (a decal pair)
    kRPinContact,  // triangles that touch or cross (an intersection line / a shared edge): equal depth along it
    kRPinNear,     // closer than the depth resolution allows at this camera (a gap smaller than thr)
    kRPinDynamic,  // one of the two is not static geometry (a stream draw)
    kRPinPending,  // the answer is not worked out yet (first sight; the budgeted work)
    kRPinNoData,   // no geometry copy / too many triangles / a projection the proof does not cover / the work cap
    // (gpu26, GXV_HSR_ORDER_TIES=7, the fast decal-aware rule) alpha-tested, a later opaque draw of the same transform that LOOKS like the base of a decal:
    kRPinBias,     // the two draws use different depth ranges (zNear / zFar): a deliberate depth bias, the game's own decal separation: kept in order
    kRPinDecal,    // the draw is a flat sheet lying on a face of the other draw's bounding box (a cheap coplanar-decal test, model-space boxes only)
    kRCount
};
inline const char* ReasonName(int r) {
    static const char* const n[kRCount] = {"movable", "cmd", "blend", "depthonly", "notest", "nowrite", "func", "mask", "cover", "stencil", "hud", "pass", "prim", "empty",
                                           "pin-xf", "pin-model", "nolater", "shader-unknown", "pin-coplanar", "pin-contact", "pin-near", "pin-dynamic", "pin-pending", "pin-nodata",
                                           "pin-bias", "pin-decal"};
    return r >= 0 && r < kRCount ? n[r] : "?";
}

// Letter of a draw for the statistics: O opaque, T alpha-tested (discard, not blended), B blended, D depth-only.
struct DrawClass {
    char letter;
    Reason why;
};
inline bool PlainDepth(const RenderState& rs) { return rs.depthTest && rs.depthWrite && (rs.depthFunc == CMP_LESS || rs.depthFunc == CMP_LEQUAL); }
// (tests only, tests/vgx_hsrorder_plan.cpp: a bit per Reason that is then NOT a barrier, the negative controls of the proof; always 0 in the game)
inline unsigned g_relax = 0;
inline DrawClass Classify(const DrawRec& d, int discard, bool inPass) { // discard: 0 no, 1 yes, 2 not known
    const RenderState& rs = d.rs;
    DrawClass c;
    c.letter = rs.blend ? 'B' : (!rs.colorWrite && !rs.alphaWrite) ? 'D' : discard == 1 ? 'T' : 'O';
    c.why = kRNone;
    if (rs.blend) c.why = kRBlend;
    else if (c.letter == 'D') c.why = kRDepthOnly;
    else if (discard == 2) c.why = kRUnknown;
    else if (inPass) c.why = kRPass;
    else if (d.prim != PRIM_TRIANGLES) c.why = kRPrim;
    else if (d.idxCount == 0) c.why = kREmpty;
    else if (d.cover || d.coverCull) c.why = kRCover;
    else if (d.stOp || d.occ) c.why = kRStencil;
    else if (d.hud) c.why = kRHud;
    else if (!rs.depthTest) c.why = kRNoTest;
    else if (!rs.depthWrite) c.why = kRNoWrite;
    else if (rs.depthFunc != CMP_LESS && rs.depthFunc != CMP_LEQUAL) c.why = kRFunc;
    else if (!rs.colorWrite || !rs.alphaWrite) c.why = kRMask;
    if (__builtin_expect(g_relax != 0, 0) && c.why != kRNone && (g_relax & (1u << c.why))) c.why = kRNone;
    return c;
}

// A hash of the draw's position matrix and projection (what its vertices' depths are computed from); equal contents hash alike. A draw
// without such blocks hashes to a fixed value (the same for all of them: treated as equal, the conservative side).
inline uint64_t TransformHash(const CmdBuf& buf, const DrawRec& d) {
    uint64_t h = 1469598103934665603ull;
    static const int blocks[2] = {UB_PROJ, UB_MV};
    for (int b = 0; b < 2; ++b) {
        const uint32_t off = d.ublock[blocks[b]], len = kUbSize[blocks[b]] > 24 ? 24 : kUbSize[blocks[b]];
        if (off == kNoBlock || off > buf.uni.size() || buf.uni.size() - off < len) { h = (h ^ 0xFFull) * 1099511628211ull; continue; }
        const uint32_t* w = reinterpret_cast<const uint32_t*>(buf.uni.data() + off);
        for (uint32_t i = 0; i < len; ++i) h = (h ^ w[i]) * 1099511628211ull;
    }
    return h ? h : 1;
}

// A set of 64-bit keys with O(1) clear (epoch stamps), a power-of-two table sized for the largest segment.
struct KeySet {
    std::vector<uint64_t> key;
    std::vector<uint32_t> stamp;
    uint32_t epoch = 1, mask = 0;
    void Reserve(size_t n) {
        size_t cap = 16;
        while (cap < n * 2 + 2) cap <<= 1;
        if (cap > key.size()) { key.assign(cap, 0); stamp.assign(cap, 0); epoch = 1; }
        mask = static_cast<uint32_t>(key.size() - 1);
    }
    void Clear() {
        if (++epoch == 0) { std::fill(stamp.begin(), stamp.end(), 0u); epoch = 1; }
    }
    bool Has(uint64_t k) const {
        uint32_t i = static_cast<uint32_t>(k * 0x9E3779B97F4A7C15ull >> 40) & mask;
        while (stamp[i] == epoch) { if (key[i] == k) return true; i = (i + 1) & mask; }
        return false;
    }
    void Add(uint64_t k) {
        uint32_t i = static_cast<uint32_t>(k * 0x9E3779B97F4A7C15ull >> 40) & mask;
        while (stamp[i] == epoch) { if (key[i] == k) return; i = (i + 1) & mask; }
        stamp[i] = epoch;
        key[i] = k;
    }
};

// ---- (ties25) GXV_HSR_ORDER_TIES=4: what decides whether an alpha-tested draw T may move behind a later opaque draw O of the same transform.
// A pair is "free" when the two cannot have equal depth at any pixel, for any reason in FreeWhy; otherwise T stays (a pin) and the reason is one of TieWhy.
enum TieWhy : uint8_t { kTFree = 0, kTCoplanar, kTContact, kTNear, kTDynamic, kTPending, kTNoData, kTBias, kTDecal };
enum FreeWhy : uint8_t { kFFunc = 0, kFInvisible, kFRect, kFDepth, kFBox, kFGap, kFSolid, kFCount };
inline const char* FreeWhyName(int r) {
    static const char* const n[kFCount] = {"func", "invisible", "rect", "depth", "box", "gap", "solid-approx"};
    return r >= 0 && r < kFCount ? n[r] : "?";
}
struct PairInfo {
    uint8_t why = kTNoData, freeWhy = 0;
    float gap = 0, thr = 0; // model-space distance found / needed (census detail)
};
struct TieOracle {
    virtual ~TieOracle() {}
    virtual void BeginPicture(const CmdBuf&) {}
    // t: an alpha-tested plain draw, o: a LATER opaque plain draw with the same transform hash and the same depth function
    virtual void Pair(const CmdBuf& buf, const DrawRec& t, const DrawRec& o, PairInfo& pi) = 0;
    // (gpu26, GXV_HSR_ORDER_TIES=7) the cheap, approximate answer: no triangle work, only cached bounding boxes; default: the exact one
    virtual void FastPair(const CmdBuf& buf, const DrawRec& t, const DrawRec& o, PairInfo& pi) { Pair(buf, t, o, pi); }
    virtual void Pump(uint64_t) {}
    virtual uint64_t GeoHashOf(const DrawRec&) { return 0; }
};
// the pin trace of one picture (census detail lines; filled only when Scratch::trace is on)
struct PinTrace { uint32_t tDraw, oDraw; uint8_t reason; float gap, thr; };

// revC: most (T, later draw) steps the TIES >= 4 scan of one picture may take (a few ms at most on the Vita); beyond it the alpha-tested draws stay in place.
constexpr uint64_t kMaxScan = 250000;

// One picture's numbers (summed over a window by the caller).
struct FrameStat {
    uint32_t scenes = 0, passDraws = 0;
    uint32_t draws = 0, cls[4] = {};            // O T B D
    uint32_t changes = 0;                        // class changes between neighbouring draws of a scene (letters O T B D)
    uint32_t tBeforeLastO = 0, bBeforeLastO = 0; // alpha-tested / blended draws that sit before the last opaque draw of their scene
    uint32_t segments = 0, segmentsWithT = 0;    // movable runs; those with an alpha-tested draw that an opaque one follows
    uint32_t tMoved = 0, oMoved = 0;
    uint64_t tMovedPos = 0;                      // sum of the positions the moved alpha-tested draws moved later
    uint32_t tLeft[kRCount] = {};                // alpha-tested draws by what kept them (kRNone: moved)
    uint32_t oLeft[kRCount] = {};                // opaque draws that are barriers, by reason
    uint32_t bLeft = 0;
    uint32_t cmdType[CMD_TYPE_COUNT] = {};       // non-draw commands (barriers) by type
    uint32_t worstTb = 0;                        // the scene with the most alpha-tested + blended draws before its last opaque draw:
    uint32_t worstDraws = 0;
    // (ties25, TIES=4) pairs (an alpha-tested draw, a later opaque one of the same transform) looked at, and the ones found free, by FreeWhy
    uint32_t pairs = 0, pairFree[kFCount] = {};
};

struct Scratch {
    std::vector<Cmd> out;               // the reordered list (Plan with apply)
    std::vector<uint8_t> letters;       // per draw of the current segment: 1 O, 2 T
    std::vector<uint64_t> xf, model;    // per draw of the segment
    std::vector<uint8_t> pinned, pinReason;
    std::vector<uint32_t> tOld;
    std::vector<Cmd> tHold;
    std::string scene;                  // the current scene's letters
    std::string worst;                  // run-length string of the scene with the most T + B before its last O
    KeySet setXf, setModel;
    std::vector<uint32_t> segStart;
    std::vector<uint64_t> xfs;          // (TIES=4) per draw of the segment
    bool traceOn = false;               // (TIES=4) fill `trace`
    std::vector<PinTrace> trace;
};

// ties: bit 0 = a T draw with the same transform (position matrix + projection contents) as a later O draw of its segment stays in place;
// bit 1 = likewise for the same static vertex buffer. 0: no pinning (exact except for coplanar draws through different transforms and
// buffers, and then only at exact depth ties); 1 (the default for GXV_HSR_ORDER=1): exact unless two different transforms give surfaces of the
// exact same depth. discard(fs): whether the fragment shader has a `discard` (0 no, 1 yes, 2 not registered yet: that draw is a barrier).
// apply: fill s.out with the new list; otherwise count only. Returns whether any command moved. The scene letters / run-length
// of the heaviest scene are kept in s.worst (census).
inline void AppendRle(std::string& dst, const std::string& letters, size_t cap) {
    for (size_t i = 0; i < letters.size() && dst.size() + 8 < cap;) {
        size_t j = i;
        while (j < letters.size() && letters[j] == letters[i]) ++j;
        dst.push_back(letters[i]);
        dst += std::to_string(j - i);
        dst.push_back(' ');
        i = j;
    }
}
inline bool Plan(const CmdBuf& buf, int (*discard)(uint16_t), unsigned ties, bool apply, Scratch& s, FrameStat& st, bool wantRle, TieOracle* oracle = nullptr) {
    const size_t n = buf.cmds.size();
    if (apply) s.out.assign(buf.cmds.begin(), buf.cmds.end());
    bool any = false, inPass = false;
    if (oracle && (ties & 4u)) oracle->BeginPicture(buf);
    s.trace.clear();
    s.scene.clear();
    size_t segA = 0, segN = 0; // the open segment: cmds [segA, segA + segN), all movable draws
    uint64_t scanLeft = kMaxScan; // (revC: the pair scan of TIES >= 4 is T x O in the worst case; a picture that exhausts it pins the rest of its alpha-tested draws: no move, no cost)
    auto flushScene = [&]() {
        if (s.scene.empty()) return;
        ++st.scenes;
        uint32_t lastO = 0;
        bool haveO = false;
        for (uint32_t i = 0; i < s.scene.size(); ++i) {
            if (i && s.scene[i] != s.scene[i - 1]) ++st.changes;
            if (s.scene[i] == 'O') { lastO = i; haveO = true; }
        }
        uint32_t tb = 0, bb = 0;
        if (haveO)
            for (uint32_t i = 0; i < lastO; ++i) { tb += s.scene[i] == 'T'; bb += s.scene[i] == 'B'; }
        st.tBeforeLastO += tb;
        st.bBeforeLastO += bb;
        if (wantRle && tb + bb > st.worstTb) {
            st.worstTb = tb + bb;
            st.worstDraws = static_cast<uint32_t>(s.scene.size());
            s.worst.clear();
            AppendRle(s.worst, s.scene, 200);
        }
        s.scene.clear();
    };
    auto flushSeg = [&]() {
        if (!segN) return;
        const size_t a = segA, m = segN;
        segN = 0;
        ++st.segments;
        // the draws of the segment
        s.letters.resize(m);
        bool anyT = false, tThenO = false, seenT = false;
        for (size_t k = 0; k < m; ++k) {
            const DrawRec& d = buf.draws[buf.cmds[a + k].arg];
            const bool t = discard(d.fsId) == 1;
            s.letters[k] = t ? 2 : 1;
            anyT |= t;
            if (t) seenT = true;
            else if (seenT) tThenO = true;
        }
        if (!anyT || !tThenO) { // nothing to exchange
            for (size_t k = 0; k < m; ++k)
                if (s.letters[k] == 2) ++st.tLeft[kRNoLater];
            return;
        }
        ++st.segmentsWithT;
        s.pinned.assign(m, 0);
        s.pinReason.assign(m, 0);
        if (ties & 4u) {
            const bool fast = (ties & 7u) == 7u; // (gpu26) GXV_HSR_ORDER_TIES=7: the fast decal-aware rule (bit 1, the static-buffer pin, is not part of it)
            // (ties25) the tighter rule: T stays only when a later opaque draw (before the next pinned T) with the same transform can tie with it
            // (same depth function: a LESS / LEQUAL pair is order-independent at ties; else the oracle: bounds, then the cached triangle distances),
            // or (bit 1) uses the same static buffer.
            s.xfs.resize(m);
            for (size_t k = 0; k < m; ++k) s.xfs[k] = TransformHash(buf, buf.draws[buf.cmds[a + k].arg]);
            size_t limit = m;
            for (size_t k = m; k-- > 0;) {
                if (s.letters[k] != 2) continue;
                const DrawRec& dt = buf.draws[buf.cmds[a + k].arg];
                uint8_t pin = 0;
                bool pending = false;
                for (size_t j = k + 1; j < limit && !pin; ++j) {
                    if (!scanLeft) { pin = kRPinNoData; break; }
                    --scanLeft;
                    if (s.letters[j] != 1) continue;
                    const DrawRec& dob = buf.draws[buf.cmds[a + j].arg];
                    if (!fast && (ties & 2u) && dt.vbuf && dt.vbuf == dob.vbuf) { pin = kRPinModel; break; }
                    if (s.xfs[j] != s.xfs[k]) continue;
                    ++st.pairs;
                    if (dt.rs.depthFunc != dob.rs.depthFunc) { ++st.pairFree[kFFunc]; continue; } // LESS vs LEQUAL: the same winner either way at a tie
                    if (!oracle) { pin = kRPinXf; break; }
                    PairInfo pi;
                    if (fast) oracle->FastPair(buf, dt, dob, pi); else oracle->Pair(buf, dt, dob, pi);
                    if (pi.why == kTFree) { ++st.pairFree[pi.freeWhy < kFCount ? pi.freeWhy : 0]; continue; }
                    if (pi.why == kTPending) { pending = true; continue; }
                    pin = pi.why == kTCoplanar ? kRPinCoplanar : pi.why == kTContact ? kRPinContact : pi.why == kTNear ? kRPinNear : pi.why == kTDynamic ? kRPinDynamic : pi.why == kTBias ? kRPinBias : pi.why == kTDecal ? kRPinDecal : kRPinNoData;
                    if (s.traceOn && s.trace.size() < 64) s.trace.push_back(PinTrace{buf.cmds[a + k].arg, buf.cmds[a + j].arg, pin, pi.gap, pi.thr});
                }
                if (!pin && pending) pin = kRPinPending;
                if (pin) {
                    s.pinned[k] = 1;
                    s.pinReason[k] = pin;
                    limit = k;
                }
            }
        } else if (ties) {
            s.setXf.Reserve(m);
            s.setModel.Reserve(m);
            s.setXf.Clear();
            s.setModel.Clear();
            for (size_t k = m; k-- > 0;) {
                const DrawRec& d = buf.draws[buf.cmds[a + k].arg];
                const uint64_t xf = TransformHash(buf, d);
                const uint64_t mk = d.vbuf ? d.vbuf : 0;
                if (s.letters[k] == 1) {
                    if (ties & 1u) s.setXf.Add(xf);
                    if ((ties & 2u) && mk) s.setModel.Add(mk);
                } else {
                    const bool px = (ties & 1u) && s.setXf.Has(xf);
                    const bool pm = (ties & 2u) && mk && s.setModel.Has(mk);
                    if (px || pm) {
                        s.pinned[k] = 1;
                        s.pinReason[k] = px ? kRPinXf : kRPinModel;
                        s.setXf.Clear();
                        s.setModel.Clear();
                    }
                }
            }
        }
        // emit: every run between pinned draws is partitioned (O in order, then T in order); a pinned draw stays at its place
        size_t w = a, k = 0;
        while (k < m) {
            s.tHold.clear();
            while (k < m && !s.pinned[k]) {
                const Cmd c = buf.cmds[a + k];
                if (s.letters[k] == 1) {
                    if (w != a + k) { ++st.oMoved; any = true; }
                    if (apply) s.out[w] = c;
                    ++w;
                } else {
                    s.tHold.push_back(c);
                    s.tOld.push_back(static_cast<uint32_t>(a + k));
                }
                ++k;
            }
            for (size_t q = 0; q < s.tHold.size(); ++q) {
                if (apply) s.out[w] = s.tHold[q];
                const size_t old = s.tOld[q];
                if (w > old) { ++st.tMoved; st.tMovedPos += w - old; ++st.tLeft[kRNone]; any = true; }
                else ++st.tLeft[kRNoLater];
                ++w;
            }
            s.tOld.clear();
            if (k < m) { // a pinned draw: in place
                if (apply) s.out[w] = buf.cmds[a + k];
                ++st.tLeft[s.pinReason[k]];
                ++w;
                ++k;
            }
        }
    };
    for (size_t i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[i];
        if (c.type != CMD_DRAW) {
            flushSeg();
            ++st.cmdType[c.type < CMD_TYPE_COUNT ? c.type : 0];
            switch (c.type) {
            case CMD_PASS_BEGIN: flushScene(); inPass = true; break;
            case CMD_COPY_TEX: {
                flushScene();
                const CopyTexCmd& t = buf.copyTex[c.arg];
                if (t.skip ? t.fromPass : !(t.srcTex || (t.passMore && t.fromPass))) inPass = false;
                break;
            }
            case CMD_COPY_DISP: case CMD_SET_TARGET: case CMD_PRESENT: flushScene(); inPass = false; break;
            default: break;
            }
            continue;
        }
        const DrawRec& d = buf.draws[c.arg];
        const DrawClass dc = Classify(d, discard(d.fsId), inPass);
        ++st.draws;
        if (inPass) { ++st.passDraws; flushSeg(); continue; } // (not counted in the scene letters: offscreen passes are their own scenes)
        s.scene.push_back(dc.letter);
        ++st.cls[dc.letter == 'O' ? 0 : dc.letter == 'T' ? 1 : dc.letter == 'B' ? 2 : 3];
        if (dc.why != kRNone) {
            flushSeg();
            if (dc.letter == 'T') ++st.tLeft[dc.why];
            else if (dc.letter == 'B') ++st.bLeft;
            else ++st.oLeft[dc.why];
            continue;
        }
        if (!segN) segA = i;
        ++segN;
    }
    flushSeg();
    flushScene();
    return any;
}

} // namespace hsr
} // namespace vgx

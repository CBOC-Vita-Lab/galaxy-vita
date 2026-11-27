// GXV_HSR_ORDER's planner (platform/vgx/src/vgx_hsrorder.h) against a software model of the GPU's per-pixel rules, no GL and no game data.
// A picture is a list of draws over an 8 x 8 grid of pixels; every draw covers a set of pixels with a depth each, an alpha-test outcome each (the
// alpha-tested ones), a colour, a depth function, depth write, blending. The model executes the list in order (fragment passes iff z <= (<) depth;
// a killed fragment writes nothing; a passing one writes depth and colour; a blended one mixes with the colour already there, an order-dependent
// function). Properties checked over many random pictures:
//   1. with every depth tie between an alpha-tested draw and a later opaque one confined to draws with the same transform (the model's "transform id"
//      selects the depth values), GXV_HSR_ORDER ties=1 gives the original picture exactly, in colour and in depth, and the planner moves something
//      in a good share of the pictures (otherwise the test proves nothing);
//   2. barriers: blended / no-depth-write / other depth function / masked / depth-only / covering / stencil / HUD draws and non-draw commands keep
//      their place and nothing crosses them (checked on the order of the output list too);
//   3. NEGATIVE CONTROLS: ties=0 with ties between alpha-tested and later opaque draws of the same transform changes pictures; and each barrier rule
//      relaxed (vgx::hsr::g_relax) changes pictures: the rules are not dead code;
//   4. the output is a permutation, opaque draws keep their order, alpha-tested draws keep their order, a pinned draw stays;
//   5. (ties25) GXV_HSR_ORDER_TIES=4: with an oracle that knows the model's geometry (a pair is free iff no pixel of both has the same depth), pictures equal
//      the original exactly and the planner moves more than ties=1; with an oracle that answers "pending" for some pairs or "dynamic" the pictures still
//      equal the original; with no oracle ties=4 is ties=1; the LESS / LEQUAL rule frees pairs of different depth functions without asking and is exact;
//      NEGATIVE CONTROLS: an oracle that calls every pair free changes pictures; so does one that is wrong only for pairs that DO tie; a plan that
//      ignores the depth function of a pair (treats equal functions as the free case) changes pictures.
//   tools/test_vgx_hsrorder_plan.sh
#include <set>
#include "vgx_hsrorder.h"
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace vgx;
namespace {
unsigned long long rngState = 88172645463325252ull;
uint32_t Rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17; return static_cast<uint32_t>(rngState >> 11); }
uint32_t U(uint32_t n) { return Rnd() % n; }

constexpr int kPx = 64;
struct DrawModel {
    uint32_t pixelMask[2] = {};   // which of the 64 pixels the draw covers
    int zBase = 0;                // depth of the pixel p = zBase + (p * 7 + zSkew) % 5 (small integers: ties are frequent)
    int zSkew = 0;
    uint64_t survive = ~0ull;      // alpha-test outcome per pixel (alpha-tested draws)
    int colour = 0;
    bool blend = false;
};
struct Picture {
    CmdBuf buf;
    std::vector<DrawModel> model;   // by draw index
    std::vector<int> cmdKind;       // per cmd: 0 draw, 1 other command
};
struct Fb {
    int c[kPx];
    int z[kPx];
    bool operator==(const Fb& o) const { return std::memcmp(c, o.c, sizeof c) == 0 && std::memcmp(z, o.z, sizeof z) == 0; }
};
// "transform id" of a draw = the first float of its MV block (the planner hashes the blocks' contents)
int XfOf(const Picture& p, const DrawRec& d) { return static_cast<int>(p.buf.uni[d.ublock[UB_MV]]); }
Fb Run(const Picture& p, const std::vector<Cmd>& cmds) {
    Fb fb;
    for (int i = 0; i < kPx; ++i) { fb.c[i] = -1; fb.z[i] = 1000000; }
    for (const Cmd& c : cmds) {
        if (c.type != CMD_DRAW) continue;
        const DrawRec& d = p.buf.draws[c.arg];
        const DrawModel& m = p.model[c.arg];
        const int xf = XfOf(p, d);
        for (int px = 0; px < kPx; ++px) {
            if (!((m.pixelMask[px >> 5] >> (px & 31)) & 1u)) continue;
            if (!((m.survive >> px) & 1ull)) continue; // (killed)
            // depth values of different transforms never tie: xf * 100 + z
            const int z = xf * 100 + m.zBase + (px * 7 + m.zSkew) % 5;
            const RenderState& rs = d.rs;
            bool pass = true;
            if (rs.depthTest) {
                switch (rs.depthFunc) {
                case CMP_LESS: pass = z < fb.z[px]; break;
                case CMP_LEQUAL: pass = z <= fb.z[px]; break;
                case CMP_GREATER: pass = z > fb.z[px]; break;
                case CMP_ALWAYS: pass = true; break;
                default: pass = false;
                }
            }
            if (!pass) continue;
            if (rs.blend) fb.c[px] = (fb.c[px] * 31 + m.colour * 17 + 5) & 0xFFFFF;
            else if (rs.colorWrite || rs.alphaWrite) fb.c[px] = (rs.colorWrite ? m.colour : fb.c[px]);
            if (rs.depthTest && rs.depthWrite) fb.z[px] = z;
            else if (!rs.depthTest && rs.depthWrite) fb.z[px] = z; // (never made: the model has no such draw)
        }
    }
    return fb;
}
int IsDiscard(uint16_t fs) { return fs & 1; }
// kinds of random draw
enum Kind { O, T, B, NoW, Func, Mask, Dep, Cover, Sten, Hud, Cmd_ };
Picture Make(int n, const int* weights, int nXf, bool tiesAcrossXf) {
    Picture p;
    (void)tiesAcrossXf;
    p.buf.uni.assign(static_cast<size_t>(nXf) * 64, 0.0f);
    for (int x = 0; x < nXf; ++x) p.buf.uni[static_cast<size_t>(x) * 64] = static_cast<float>(x); // block x: MV; the projection is the same for all
    int total = 0;
    for (int k = 0; k < Cmd_ + 1; ++k) total += weights[k];
    for (int i = 0; i < n; ++i) {
        int r = static_cast<int>(U(static_cast<uint32_t>(total))), k = 0;
        while (r >= weights[k]) { r -= weights[k]; ++k; }
        if (k == Cmd_) { p.buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, 0}); continue; }
        DrawRec d;
        DrawModel m;
        d.prim = PRIM_TRIANGLES;
        d.idxCount = 3;
        d.vsId = 1;
        d.fsId = static_cast<uint16_t>(2 * U(8) + (k == T ? 1 : 0));
        const int xf = static_cast<int>(U(static_cast<uint32_t>(nXf)));
        d.ublock[UB_MV] = static_cast<uint32_t>(xf) * 64;
        d.ublock[UB_PROJ] = static_cast<uint32_t>(xf) * 64 + 32; // (zeros: equal for all draws; the hash includes the block contents of MV only in effect)
        d.vbuf = static_cast<uint32_t>(1 + U(3));
        d.rs.depthTest = 1; d.rs.depthWrite = 1; d.rs.depthFunc = U(2) ? CMP_LEQUAL : CMP_LESS;
        d.rs.colorWrite = d.rs.alphaWrite = 1;
        m.colour = static_cast<int>(1 + Rnd() % 1000);
        m.zBase = static_cast<int>(U(4));
        m.zSkew = static_cast<int>(U(5));
        const int nPix = 4 + static_cast<int>(U(40));
        for (int j = 0; j < nPix; ++j) { const uint32_t px = U(kPx); m.pixelMask[px >> 5] |= 1u << (px & 31); }
        if (k == T) m.survive = (static_cast<uint64_t>(Rnd()) << 32) ^ Rnd() ^ (static_cast<uint64_t>(Rnd()) << 11);
        switch (k) {
        case B: d.rs.blend = 1; d.rs.depthWrite = U(2); m.blend = true; break;
        case NoW: d.rs.depthWrite = 0; break;
        case Func: d.rs.depthFunc = CMP_GREATER; break;
        case Mask: d.rs.colorWrite = 0; break;
        case Dep: d.rs.colorWrite = d.rs.alphaWrite = 0; break;
        case Cover: d.cover = 1; d.rs.depthFunc = CMP_ALWAYS; m.pixelMask[0] = m.pixelMask[1] = ~0u; m.zBase = 0; m.zSkew = 0; break;
        case Sten: d.stOp = 2; break;
        case Hud: d.hud = 1; break;
        default: break;
        }
        p.model.resize(p.buf.draws.size() + 1);
        p.model[p.buf.draws.size()] = m;
        p.buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(p.buf.draws.size())});
        p.buf.draws.push_back(d);
    }
    return p;
}
// the checks of item 2/4 on one plan
bool CheckStructure(const Picture& p, const std::vector<Cmd>& out, unsigned ties, std::string& why) {
    const auto& in = p.buf.cmds;
    if (out.size() != in.size()) { why = "size"; return false; }
    // permutation
    std::map<uint64_t, int> cnt;
    for (const Cmd& c : in) ++cnt[static_cast<uint64_t>(c.type) << 32 | c.arg];
    for (const Cmd& c : out) --cnt[static_cast<uint64_t>(c.type) << 32 | c.arg];
    for (auto& kv : cnt) if (kv.second) { why = "not a permutation"; return false; }
    // barriers keep their position
    for (size_t i = 0; i < in.size(); ++i) {
        bool barrier = in[i].type != CMD_DRAW;
        if (!barrier) {
            const DrawRec& d = p.buf.draws[in[i].arg];
            const hsr::DrawClass dc = hsr::Classify(d, IsDiscard(d.fsId), false);
            barrier = dc.why != hsr::kRNone;
        }
        if (barrier && (out[i].type != in[i].type || out[i].arg != in[i].arg)) { why = "a barrier moved"; return false; }
    }
    // relative order inside each class
    std::vector<uint32_t> oi, ti, oo, to;
    for (const Cmd& c : in) if (c.type == CMD_DRAW) (IsDiscard(p.buf.draws[c.arg].fsId) == 1 ? ti : oi).push_back(c.arg);
    for (const Cmd& c : out) if (c.type == CMD_DRAW) (IsDiscard(p.buf.draws[c.arg].fsId) == 1 ? to : oo).push_back(c.arg);
    // (barrier draws of either class are in place, so the classes' order among the movable ones is what is kept: compare the subsequences of movable draws)
    auto movable = [&](uint32_t di) { const DrawRec& d = p.buf.draws[di]; return hsr::Classify(d, IsDiscard(d.fsId), false).why == hsr::kRNone; };
    std::vector<uint32_t> a, b;
    for (uint32_t x : oi) if (movable(x)) a.push_back(x);
    for (uint32_t x : oo) if (movable(x)) b.push_back(x);
    if (a != b) { why = "opaque order"; return false; }
    a.clear(); b.clear();
    for (uint32_t x : ti) if (movable(x)) a.push_back(x);
    for (uint32_t x : to) if (movable(x)) b.push_back(x);
    if (a != b) { why = "alpha-tested order"; return false; }
    (void)ties;
    return true;
}
} // namespace

// the oracle for the model: the geometry of a draw is its pixel mask and depth formula (the alpha-test outcome is not geometry)
struct ModelOracle : hsr::TieOracle {
    const Picture* pic = nullptr;
    int mode = 0; // 0 exact, 1 every pair free (wrong), 2 free unless the two overlap in pixels (wrong: ignores equal depth only where they differ... see below), 3 pending for pairs with even indices, 4 dynamic for all
    unsigned calls = 0, freed = 0;
    void Pair(const CmdBuf& buf, const DrawRec& t, const DrawRec& o, hsr::PairInfo& pi) override {
        ++calls;
        const uint32_t it = static_cast<uint32_t>(&t - buf.draws.data()), io = static_cast<uint32_t>(&o - buf.draws.data());
        const DrawModel& a = pic->model[it];
        const DrawModel& b = pic->model[io];
        pi = hsr::PairInfo{};
        if (mode == 4) { pi.why = hsr::kTDynamic; return; }
        if (mode == 3 && ((it + io) & 1u)) { pi.why = hsr::kTPending; return; }
        bool tie = false;
        for (int px = 0; px < kPx && !tie; ++px) {
            if (!((a.pixelMask[px >> 5] >> (px & 31)) & 1u) || !((b.pixelMask[px >> 5] >> (px & 31)) & 1u)) continue;
            tie = (a.zBase + (px * 7 + a.zSkew) % 5) == (b.zBase + (px * 7 + b.zSkew) % 5); // (same transform: the same xf * 100)
        }
        if (mode == 1) tie = false;
        if (mode == 2) tie = tie && (U(3) != 0); // (wrong for a third of the pairs that do tie)
        if (tie) { pi.why = hsr::kTContact; return; }
        pi.why = hsr::kTFree; pi.freeWhy = hsr::kFGap; ++freed;
    }
};

int main() {
    unsigned failures = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; };
    hsr::Scratch scr;
    // weights: O T B nowrite func mask depthonly cover stencil hud cmd
    const int mix[Cmd_ + 1] = {40, 28, 6, 3, 2, 2, 2, 2, 1, 1, 3};
    const int mixAll[Cmd_ + 1] = {30, 25, 12, 8, 8, 6, 6, 5, 4, 4, 6};
    const int trials = 4000;
    unsigned moved = 0, same1 = 0, diff1 = 0, structBad = 0;
    unsigned diffTies0 = 0, diff3 = 0;
    std::string why;
    // 1/2/4: ties = 1 (transform pins), ties across draws of the same transform only (the model puts different transforms at different depths)
    for (int t = 0; t < trials; ++t) {
        Picture p = Make(10 + static_cast<int>(U(60)), t % 2 ? mix : mixAll, 3, false);
        hsr::FrameStat st;
        const bool mv = hsr::Plan(p.buf, IsDiscard, 1, true, scr, st, false);
        moved += mv;
        const Fb a = Run(p, p.buf.cmds), b = Run(p, scr.out);
        if (a == b) ++same1; else ++diff1;
        if (!CheckStructure(p, scr.out, 1, why)) { ++structBad; if (structBad < 4) std::printf("structure: %s (trial %d)\n", why.c_str(), t); }
        hsr::Plan(p.buf, IsDiscard, 3, true, scr, st, false); // (ties 3 pins more: still exact)
        if (!(Run(p, scr.out) == a)) ++diff3;
        if (!CheckStructure(p, scr.out, 3, why)) ++structBad;
        // 3. ties = 0
        hsr::Plan(p.buf, IsDiscard, 0, true, scr, st, false);
        if (!(Run(p, scr.out) == a)) ++diffTies0;
    }
    std::printf("trials %d: reordered %u, equal %u, different %u, structure errors %u; ties=0 pictures that differ: %u\n", trials, moved, same1, diff1, structBad, diffTies0);
    check(diff1 == 0, "ties=1: every reordered picture equals the original (colour and depth)");
    check(diff3 == 0, "ties=3 (transform and static-buffer pins): every picture equals the original");
    check(structBad == 0, "plan structure: permutation, barriers in place, class orders kept");
    check(moved > trials / 3, "the planner moves something in a good share of the pictures");
    check(diffTies0 > 0, "NEGATIVE CONTROL: ties=0 changes some pictures (equal-depth pairs of one transform: the pin is needed)");
    // 3. negative controls: every barrier rule relaxed changes pictures
    struct Relax { int reason; const char* name; } relax[] = {{hsr::kRBlend, "blended"}, {hsr::kRNoWrite, "no depth write"}, {hsr::kRFunc, "other depth function"},
                                                              {hsr::kRMask, "colour mask"}, {hsr::kRDepthOnly, "depth-only"}, {hsr::kRCover, "covering quad"}};
    for (const Relax& r : relax) {
        hsr::g_relax = 1u << r.reason;
        unsigned diff = 0;
        for (int t = 0; t < 1500; ++t) {
            Picture p = Make(20 + static_cast<int>(U(40)), mixAll, 3, false);
            hsr::FrameStat st;
            hsr::Plan(p.buf, IsDiscard, 1, true, scr, st, false);
            if (!(Run(p, p.buf.cmds) == Run(p, scr.out))) ++diff;
        }
        hsr::g_relax = 0;
        char msg[160];
        std::snprintf(msg, sizeof msg, "NEGATIVE CONTROL: rule \"%s\" relaxed changes pictures (%u of 1500)", r.name, diff);
        check(diff > 0, msg);
    }
    // 5. ties = 4
    {
        unsigned diff4 = 0, diffNo = 0, diffPend = 0, diffDyn = 0, moreMoved = 0, lessEq = 0, structBad4 = 0, diffLie = 0, diffWrong = 0, diff6 = 0, diff7 = 0, structBad7 = 0, moved7 = 0;
        unsigned movedT1 = 0, movedT4 = 0, freedPairs = 0, pairs = 0;
        const int n4 = 4000;
        for (int t = 0; t < n4; ++t) {
            Picture p = Make(10 + static_cast<int>(U(60)), t % 2 ? mix : mixAll, 3, false);
            const Fb a = Run(p, p.buf.cmds);
            hsr::FrameStat s1, s4;
            hsr::Plan(p.buf, IsDiscard, 1, true, scr, s1, false);
            ModelOracle orc;
            orc.pic = &p;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, s4, false, &orc);
            if (!(Run(p, scr.out) == a)) ++diff4;
            if (!CheckStructure(p, scr.out, 4, why)) ++structBad4;
            movedT1 += s1.tMoved; movedT4 += s4.tMoved;
            if (s4.tMoved > s1.tMoved) ++moreMoved;
            if (s4.tMoved >= s1.tMoved) ++lessEq;
            freedPairs += orc.freed; pairs += s4.pairs;
            hsr::FrameStat sn;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, sn, false, nullptr); // (no oracle: the xf pin, but the depth-function rule still frees pairs)
            if (!(Run(p, scr.out) == a)) ++diffNo;
            hsr::FrameStat sp;
            orc.mode = 3;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, sp, false, &orc);
            if (!(Run(p, scr.out) == a)) ++diffPend;
            orc.mode = 4;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, sp, false, &orc);
            if (!(Run(p, scr.out) == a)) ++diffDyn;
            orc.mode = 0;
            hsr::Plan(p.buf, IsDiscard, 6, true, scr, sp, false, &orc); // (4 + 2: the same static buffer pins too)
            if (!(Run(p, scr.out) == a)) ++diff6;
            { // (gpu26) ties=7 (the fast rule): through an oracle whose FastPair is the exact model answer the picture equals the original and the structure holds
                hsr::FrameStat s7;
                hsr::Plan(p.buf, IsDiscard, 7, true, scr, s7, false, &orc);
                std::string why7;
                if (!(Run(p, scr.out) == a)) ++diff7;
                if (!CheckStructure(p, scr.out, 7, why7)) ++structBad7;
                moved7 += s7.tMoved;
                orc.mode = 4;
                hsr::Plan(p.buf, IsDiscard, 7, true, scr, s7, false, &orc);
                if (!(Run(p, scr.out) == a)) ++diff7;
                orc.mode = 0;
            }
            // negative controls
            orc.mode = 1;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, sp, false, &orc);
            if (!(Run(p, scr.out) == a)) ++diffLie;
            orc.mode = 2;
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, sp, false, &orc);
            if (!(Run(p, scr.out) == a)) ++diffWrong;
        }
        char msg[300];
        std::snprintf(msg, sizeof msg, "ties=4 with the geometry oracle: every picture equals the original (%d pictures; T moved %u vs %u for ties=1, more in %u; %u of %u same-transform pairs freed)", n4, movedT4, movedT1, moreMoved, freedPairs, pairs);
        check(diff4 == 0, msg);
        check(structBad4 == 0, "ties=4 plan structure: permutation, barriers in place, class orders kept");
        check(diffNo == 0, "ties=4 without an oracle (the xf pin; the depth-function rule on) equals the original");
        check(diffPend == 0 && diffDyn == 0, "ties=4 with pairs answered pending / dynamic (pinned): equals the original");
        check(diff6 == 0, "ties=6 (oracle + same-static-buffer pin): equals the original");
        std::snprintf(msg, sizeof msg, "ties=7 (fast rule, exact stand-in oracle / all dynamic): equals the original; structure ok (T moved %u)", moved7);
        check(diff7 == 0 && structBad7 == 0 && moved7 > 0, msg);
        check(moreMoved > static_cast<unsigned>(n4) / 10 && movedT4 > movedT1, "ties=4 moves more alpha-tested draws than ties=1 (the rule frees something)");
        std::snprintf(msg, sizeof msg, "NEGATIVE CONTROL: an oracle that calls every pair free changes pictures (%u of %d)", diffLie, n4);
        check(diffLie > 0, msg);
        std::snprintf(msg, sizeof msg, "NEGATIVE CONTROL: an oracle wrong for a third of the pairs that tie changes pictures (%u of %d)", diffWrong, n4);
        check(diffWrong > 0, msg);
        (void)lessEq;
        // the depth-function rule is exact on its own AND needed: LESS vs LEQUAL pairs at equal depth give the same picture either way, equal functions do not
        unsigned diffSame = 0, diffMixed = 0, triedSame = 0, triedMixed = 0;
        for (int t = 0; t < 3000; ++t) {
            Picture p;
            p.buf.uni.assign(128, 0.0f);
            auto add = [&](uint16_t fs, int colour, int func, int zb) {
                DrawRec d; d.prim = PRIM_TRIANGLES; d.idxCount = 3; d.fsId = fs; d.rs.depthTest = d.rs.depthWrite = 1; d.rs.depthFunc = static_cast<uint8_t>(func); d.rs.colorWrite = d.rs.alphaWrite = 1;
                d.ublock[UB_MV] = 0; d.ublock[UB_PROJ] = 32;
                DrawModel m; m.colour = colour; m.pixelMask[0] = 0xFFFFFFFFu; m.pixelMask[1] = 0xFFFFFFFFu; m.zBase = zb; m.survive = (static_cast<uint64_t>(Rnd()) << 32) ^ Rnd();
                p.model.push_back(m);
                p.buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(p.buf.draws.size())});
                p.buf.draws.push_back(d);
            };
            const int ft = U(2) ? CMP_LEQUAL : CMP_LESS, fo = U(2) ? CMP_LEQUAL : CMP_LESS;
            add(1, 11, ft, 1); add(0, 22, fo, 1); // an alpha-tested draw then an opaque one, every pixel at the same depth (all pixels tie)
            hsr::FrameStat st;
            const Fb a = Run(p, p.buf.cmds);
            hsr::Plan(p.buf, IsDiscard, 4, true, scr, st, false, nullptr);
            const bool same = Run(p, scr.out) == a;
            if (ft == fo) { ++triedSame; if (!same) ++diffSame; } else { ++triedMixed; if (!same) ++diffMixed; }
        }
        std::snprintf(msg, sizeof msg, "depth functions at a total tie: LESS vs LEQUAL pairs freed and exact (%u pairs, %u differ); equal functions stay pinned and exact (%u pairs, %u differ)", triedMixed, diffMixed, triedSame, diffSame);
        check(diffMixed == 0 && diffSame == 0 && triedMixed > 0 && triedSame > 0, msg);
        // NEGATIVE CONTROL: the rule applied to pairs of EQUAL functions too would change pictures: the same pair, moved by force
        unsigned diffForce = 0;
        for (int t = 0; t < 400; ++t) {
            Picture p;
            p.buf.uni.assign(128, 0.0f);
            for (int i = 0; i < 2; ++i) {
                DrawRec d; d.prim = PRIM_TRIANGLES; d.idxCount = 3; d.fsId = static_cast<uint16_t>(1 - i); d.rs.depthTest = d.rs.depthWrite = 1; d.rs.depthFunc = CMP_LEQUAL; d.rs.colorWrite = d.rs.alphaWrite = 1;
                d.ublock[UB_MV] = 0; d.ublock[UB_PROJ] = 32;
                DrawModel m; m.colour = 11 * (i + 1); m.pixelMask[0] = m.pixelMask[1] = 0xFFFFFFFFu; m.zBase = 1; m.survive = ~0ull;
                p.model.push_back(m);
                p.buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(i)});
                p.buf.draws.push_back(d);
            }
            hsr::FrameStat st;
            hsr::Plan(p.buf, IsDiscard, 0, true, scr, st, false, nullptr); // ties 0: no pin at all: the T (LEQUAL) moves behind the O (LEQUAL): the tie flips
            if (!(Run(p, scr.out) == Run(p, p.buf.cmds))) ++diffForce;
        }
        std::snprintf(msg, sizeof msg, "NEGATIVE CONTROL: moving a LEQUAL alpha-tested draw behind a LEQUAL opaque one at a tie changes the picture (%u of 400)", diffForce);
        check(diffForce == 400, msg);
    }
    // a non-draw command between the draws splits segments: with no draws crossing it
    {
        Picture p;
        p.buf.uni.assign(128, 0.0f);
        auto add = [&](uint16_t fs, int colour) {
            DrawRec d; d.prim = PRIM_TRIANGLES; d.idxCount = 3; d.fsId = fs; d.rs.depthTest = d.rs.depthWrite = 1; d.rs.depthFunc = CMP_LEQUAL; d.rs.colorWrite = d.rs.alphaWrite = 1;
            d.ublock[UB_MV] = 0; d.ublock[UB_PROJ] = 32;
            DrawModel m; m.colour = colour; m.pixelMask[0] = 0xFFFF;
            p.model.push_back(m);
            p.buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(p.buf.draws.size())});
            p.buf.draws.push_back(d);
        };
        add(1, 1); add(0, 2); p.buf.cmds.push_back(Cmd{CMD_TEX_UPLOAD, 0}); add(0, 3); add(1, 4); add(0, 5);
        hsr::FrameStat st;
        hsr::Plan(p.buf, IsDiscard, 0, true, scr, st, false);
        // expect: [T1 O2 | upload | O3 T4 O5] -> [O2 T1 | upload | O3 O5 T4]
        const uint32_t want[] = {1, 0, 99, 2, 4, 3};
        bool ok = scr.out.size() == 6;
        for (size_t i = 0; ok && i < 6; ++i) ok = want[i] == 99 ? scr.out[i].type != CMD_DRAW : (scr.out[i].type == CMD_DRAW && scr.out[i].arg == want[i]);
        check(ok, "a non-draw command splits segments; opaque first inside each (T O | cmd | O T O -> O T | cmd | O O T)");
        check(st.tMoved == 2, "two alpha-tested draws moved later");
    }
    // revC: the pair scan is bounded. 1000 alpha-tested draws followed by 1000 opaque ones of one transform, all disjoint (every pair free): the scan would take 1M steps;
    // past hsr::kMaxScan the rest are pinned (kRPinNoData), the picture is still a valid permutation and equal to the original; a small picture is not capped at all.
    for (int big = 0; big < 2; ++big) {
        const int nT = big ? 1000 : 40;
        Picture p;
        p.buf.uni.assign(128, 0.0f);
        auto add = [&](uint16_t fs, int colour, int px) {
            DrawRec d; d.prim = PRIM_TRIANGLES; d.idxCount = 3; d.fsId = fs; d.rs.depthTest = d.rs.depthWrite = 1; d.rs.depthFunc = CMP_LESS; d.rs.colorWrite = d.rs.alphaWrite = 1;
            d.ublock[UB_MV] = 0; d.ublock[UB_PROJ] = 32;
            DrawModel m; m.colour = colour; m.pixelMask[px >> 5] = 1u << (px & 31); m.zBase = 1 + (colour & 3);
            p.model.push_back(m);
            p.buf.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(p.buf.draws.size())});
            p.buf.draws.push_back(d);
        };
        for (int i = 0; i < nT; ++i) add(1, 1 + i, i % 32);
        for (int i = 0; i < nT; ++i) add(0, 2000 + i, 32 + i % 32);
        ModelOracle orc;
        orc.pic = &p;
        hsr::FrameStat st;
        hsr::Plan(p.buf, IsDiscard, 4, true, scr, st, false, &orc);
        std::set<uint32_t> seen;
        for (const Cmd& c : scr.out) seen.insert(c.arg);
        const bool perm = scr.out.size() == p.buf.cmds.size() && seen.size() == p.buf.cmds.size();
        check(perm && Run(p, scr.out) == Run(p, p.buf.cmds), big ? "a huge picture: still a permutation, equal to the original" : "a small picture: equal to the original");
        if (big) check(st.tLeft[hsr::kRPinNoData] > 0 && orc.calls < 2 * hsr::kMaxScan, "the scan is capped: some alpha-tested draws pinned (nodata), oracle calls bounded");
        else check(st.tLeft[hsr::kRPinNoData] == 0 && st.tMoved == static_cast<uint32_t>(nT), "a small picture is not capped: every alpha-tested draw moves");
    }
    std::printf("%s\n", failures ? "FAILED" : "PASS: hsrorder planner");
    return failures ? 1 : 0;
}

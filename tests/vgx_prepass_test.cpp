// GXV_ALPHATEST_PREPASS (platform/vgx/src/vgx_prepass.h): host test of the pure parts and of the exactness argument, on a model of the depth / colour buffers.
//  * TextOk: the prepass key's text must be the tested text up to and including the test line + the constant output;
//  * StateOk;
//  * PlanHoist: segments, what is hoisted;
//  * the model: random pictures (a few pixels, depths 0..2 so that ties are common, tested draws with some killed fragments, LESS / LEQUAL draws, barriers that are
//    blended draws) drawn in the original order, as in-place pairs and with the planner's hoists; the buffers must come out alike.
//    keepTest (modes 4 / 5: the colour pass keeps its test): exact in every picture (tested draws of function LEQUAL are split; LESS ones are drawn as before). Without it (modes 1 / 2): exact when no fragment is killed, and NOT exact where a killed
//    fragment ties in depth with another surface (the test counts the pictures that differ: a documented limit, it must be non-zero or the model has no coplanar decals).
//  * negative controls: splitting (and hoisting) a LESS draw, hoisting across a barrier (a draw whose depth function is ALWAYS) and a colour pass that ignores the depth each change a picture.
#include "../platform/vgx/src/vgx_prepass.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned failures = 0, checks = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
using namespace vgx;
using namespace vgx::atpp;

namespace {
constexpr int kPix = 3;
struct Frag { int z = -1; bool alive = true; }; // z < 0: the draw does not cover the pixel
struct Draw {
    char kind = 'O';          // O opaque, T tested, B barrier (writes colour, depth test LEQUAL, no depth write), A barrier (depth function ALWAYS: writes colour and depth)
    bool less = false;        // depth function LESS (else LEQUAL)
    Frag f[kPix];
};
struct Buf { int depth[kPix]; int colour[kPix]; Buf() { for (int i = 0; i < kPix; ++i) { depth[i] = 9; colour[i] = -1; } } };
bool Pass(bool less, int z, int d) { return less ? z < d : z <= d; }
// the original draw
void Orig(Buf& b, const Draw& d, int id) {
    for (int p = 0; p < kPix; ++p) {
        const Frag& f = d.f[p];
        if (f.z < 0 || (d.kind == 'T' && !f.alive)) continue;
        if (d.kind == 'B') { if (Pass(false, f.z, b.depth[p])) b.colour[p] = id; continue; }
        if (d.kind == 'A') { b.depth[p] = f.z; b.colour[p] = id; continue; }
        if (Pass(d.less, f.z, b.depth[p])) { b.depth[p] = f.z; b.colour[p] = id; }
    }
}
// the prepass: depth only; the colour pass: EQUAL, no depth write, with or without the test (keepTest), or a LEQUAL one (the negative control)
void Prepass(Buf& b, const Draw& d) {
    for (int p = 0; p < kPix; ++p) {
        const Frag& f = d.f[p];
        if (f.z < 0 || !f.alive) continue;
        if (Pass(d.less, f.z, b.depth[p])) b.depth[p] = f.z;
    }
}
void Colour(Buf& b, const Draw& d, int id, bool keepTest, bool always) {
    for (int p = 0; p < kPix; ++p) {
        const Frag& f = d.f[p];
        if (f.z < 0 || (keepTest && !f.alive)) continue;
        if (always || f.z == b.depth[p]) b.colour[p] = id;
    }
}
struct Opt { bool keepTest = true, hoist = false, hoistLess = false, hoistOverBarrier = false, always = false; };
Buf Run(const std::vector<Draw>& s, const Opt& o) {
    Buf b;
    const size_t n = s.size();
    std::vector<uint8_t> kinds(n);
    for (size_t i = 0; i < n; ++i) kinds[i] = (s[i].kind == 'B' || s[i].kind == 'A') ? kBarrier : s[i].kind == 'O' ? kOpaque : (s[i].less && !o.hoistLess) ? kSplit : kHoistable;
    if (o.hoistOverBarrier)
        for (size_t i = 0; i < n; ++i) if (kinds[i] == kBarrier) kinds[i] = kOpaque; // (the control: the planner takes the barriers for plain opaque draws)
    std::vector<Entry> plan;
    Stat st;
    if (o.hoist) PlanHoist(kinds.data(), n, plan, st);
    std::vector<uint8_t> hoisted(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for (const Entry& e : plan)
            if (e.at == i) { Prepass(b, s[e.cmd]); hoisted[e.cmd] = 1; }
        const Draw& d = s[i];
        if (d.kind == 'T' && (!d.less || o.hoistLess)) { // (a LESS tested draw is not split: StateOk)
            if (!hoisted[i]) Prepass(b, d);
            Colour(b, d, static_cast<int>(i), o.keepTest, o.always);
        } else {
            Orig(b, d, static_cast<int>(i));
        }
    }
    return b;
}
bool Same(const Buf& a, const Buf& b) { return std::memcmp(a.depth, b.depth, sizeof a.depth) == 0 && std::memcmp(a.colour, b.colour, sizeof a.colour) == 0; }
Buf Original(const std::vector<Draw>& s) {
    Buf b;
    for (size_t i = 0; i < s.size(); ++i) Orig(b, s[i], static_cast<int>(i));
    return b;
}
uint64_t rng = 88172645463325252ull;
unsigned R(unsigned n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<unsigned>(rng % n); }
std::vector<Draw> Scene(bool allowKill, bool allowLessT, bool barriers) {
    std::vector<Draw> s(3 + R(6));
    for (Draw& d : s) {
        const unsigned k = R(11);
        d.kind = (barriers && k == 0) ? 'B' : (barriers && k == 10) ? 'A' : k < 5 ? 'O' : 'T';
        d.less = (d.kind == 'O' || d.kind == 'B') ? R(4) == 0 : (allowLessT && R(3) == 0);
        if (d.kind == 'B' || d.kind == 'A') d.less = false;
        for (Frag& f : d.f) {
            f.z = R(4) == 0 ? -1 : static_cast<int>(R(3));
            f.alive = !(d.kind == 'T' && allowKill && R(3) == 0);
        }
    }
    return s;
}
} // namespace

int main() {
    // ---- text
    const std::string orig = "void main() {\n  float ax = fin.a * 255.0 + 0.5;\n  if (!((ax >= u_tev[8].x) && (ax < u_tev[8].y + 1.0))) discard;\n  fin.a = u_tev[8].z;\n  gl_FragColor = fin;\n}\n";
    const std::string pre = "void main() {\n  float ax = fin.a * 255.0 + 0.5;\n  if (!((ax >= u_tev[8].x) && (ax < u_tev[8].y + 1.0))) discard;\n  gl_FragColor = vec4(0.0);\n}\n";
    Check(TextOk(orig, pre), "TextOk accepts the tested text cut at its test");
    Check(!TextOk(orig, orig), "TextOk refuses the tested text itself");
    { std::string bad = pre; bad.replace(bad.find("255.0"), 5, "254.0"); Check(!TextOk(orig, bad), "TextOk refuses a different alpha expression"); }
    { std::string bad = pre; bad.replace(bad.find("vec4(0.0)"), 9, "fin"); Check(!TextOk(orig, bad), "TextOk refuses an output that is not the constant"); }
    Check(!TextOk("void main() {\n  gl_FragColor = fin;\n}\n", pre), "TextOk refuses a text without a test");
    Check(!TextOk(orig, pre + "  \n"), "TextOk refuses trailing text");
    // ---- state
    RenderState rs;
    rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_LEQUAL;
    Check(StateOk(rs), "StateOk: plain LEQUAL");
    rs.depthFunc = CMP_LESS; Check(!StateOk(rs), "StateOk: LESS refused");
    rs.depthFunc = CMP_EQUAL; Check(!StateOk(rs), "StateOk: EQUAL refused");
    rs.depthFunc = CMP_LEQUAL; rs.blend = 1; Check(!StateOk(rs), "StateOk: blend refused");
    rs.blend = 0; rs.depthWrite = 0; Check(!StateOk(rs), "StateOk: no depth write refused");
    rs.depthWrite = 1; rs.colorWrite = 0; Check(!StateOk(rs), "StateOk: colour mask refused");
    // ---- planner
    {
        const uint8_t k[] = {kOpaque, kHoistable, kHoistable, kBarrier, kHoistable, kOpaque, kHoistable, kSplit, kBarrier, kBarrier, kOpaque, kHoistable};
        std::vector<Entry> out;
        Stat st;
        PlanHoist(k, sizeof k, out, st);
        Check(out.size() == 4 && out[0].at == 0 && out[0].cmd == 1 && out[1].cmd == 2 && out[2].at == 4 && out[2].cmd == 6 && out[3].at == 10 && out[3].cmd == 11, "PlanHoist: entries at their segment's start, only after an opaque draw");
        Check(st.segments == 3 && st.segmentsWithHoist == 3 && st.hoisted == 4 && st.inPlace == 2, "PlanHoist: counts");
        const uint8_t none[] = {kHoistable, kHoistable, kOpaque};
        out.clear(); st = Stat{};
        PlanHoist(none, sizeof none, out, st);
        Check(out.empty() && st.inPlace == 2, "PlanHoist: a tested draw before the first opaque draw stays in place");
    }
    // ---- the model
    unsigned exactKeep = 0, exactNoKill = 0, differNoKeep = 0, ctlLess = 0, ctlBarrier = 0, ctlLeq = 0, n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0, n6 = 0, bad1 = 0, bad2 = 0, bad3 = 0;
    for (int i = 0; i < 60000; ++i) {
        { // keepTest: in place (LESS and LEQUAL tested draws) and hoisted (LEQUAL only: LESS ones are kSplit), with barriers
            const std::vector<Draw> s = Scene(true, true, true);
            const Buf ref = Original(s);
            Opt a; a.keepTest = true; a.hoist = false;
            Opt b; b.keepTest = true; b.hoist = true;
            ++n1; if (!Same(ref, Run(s, a))) ++bad1; else ++exactKeep;
            ++n2; if (!Same(ref, Run(s, b))) ++bad2; else ++exactKeep;
        }
        { // no kill: the colour pass without the test is exact too
            const std::vector<Draw> s = Scene(false, true, true);
            const Buf ref = Original(s);
            Opt a; a.keepTest = false; a.hoist = false;
            Opt b; b.keepTest = false; b.hoist = true;
            ++n3; if (!Same(ref, Run(s, a))) ++bad3; else ++exactNoKill;
            if (!Same(ref, Run(s, b))) ++bad3; else ++exactNoKill;
        }
        { // with kills and no test in the colour pass: some pictures differ (a killed fragment tying with another surface)
            const std::vector<Draw> s = Scene(true, false, true);
            Opt a; a.keepTest = false; a.hoist = true;
            ++n4; if (!Same(Original(s), Run(s, a))) ++differNoKeep;
        }
        { // negative controls
            const std::vector<Draw> s = Scene(true, true, true);
            const Buf ref = Original(s);
            Opt a; a.keepTest = true; a.hoist = true; a.hoistLess = true;
            ++n5; if (!Same(ref, Run(s, a))) ++ctlLess;
            Opt b; b.keepTest = true; b.hoist = true; b.hoistOverBarrier = true;
            ++n6; if (!Same(ref, Run(s, b))) ++ctlBarrier;
            Opt c; c.keepTest = true; c.hoist = false; c.always = true;
            if (!Same(ref, Run(s, c))) ++ctlLeq;
        }
    }
    std::printf("model: %u pictures keepTest in place: %u differ; %u hoisted: %u differ; no kills, no test in the colour pass: %u differ of %u; with kills and no test: %u of %u differ (the documented limit)\n",
                n1, bad1, n2, bad2, bad3, n3 * 2, differNoKeep, n4);
    std::printf("controls: splitting LESS draws changes %u of %u, hoisting across barriers %u of %u, a colour pass that ignores the depth %u of %u\n", ctlLess, n5, ctlBarrier, n6, ctlLeq, n5);
    Check(bad1 == 0, "model: the pair in place with the test kept in the colour pass reproduces every picture");
    Check(bad2 == 0, "model: the hoisted prepass with the test kept reproduces every picture (barriers, LESS and LEQUAL draws, ties)");
    Check(bad3 == 0, "model: with no killed fragment the colour pass without the test reproduces every picture (in place and hoisted)");
    Check(differNoKeep > 0, "model: a colour pass without the test is NOT exact where a killed fragment ties in depth with another surface (the limit the modes 1 / 2 carry)");
    Check(ctlLess > 0, "negative control: splitting a LESS draw changes pictures");
    Check(ctlBarrier > 0, "negative control: hoisting across a barrier changes pictures");
    Check(ctlLeq > 0, "negative control: a colour pass that ignores the depth changes pictures");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

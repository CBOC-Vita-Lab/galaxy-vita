// halo31 (18 Nov 2026): GXV_HRW_HALO's draws in the REAL back end, on the GL stub (the keepdepth31 harness, built as the console builds it). The stub's state
// calls are recorded per draw, so the test reads what each submission would be drawn with:
//   - a buffer executed as the pair's fresh buffer with two bands armed: every EFB draw is submitted three times: twice into the stencil only (colour and depth
//     masked, EQUAL 0 -> INCR, the band's scissor, the viewport moved by the band's vector) and once as it is with the stencil set to 2 (pass or depth fail),
//     the stencil test off after it, the draw's own viewport, scissor, colour mask and depth state back;
//   - the same buffer with nothing armed: one submission a draw, the stencil never touched (the switch off costs nothing);
//   - the scene pass's two extra parts (DrawWarp 3 and 4): the reset writes 0 inside the plan's rectangle with the colour masked, the fill is drawn once per
//     band where the stencil is 1, in the band's scissor, the viewport moved by the band's fill vector, with the colour on.
// tools/test_halo31.sh runs it. (The pair's own flow, WarpPrepare to the display copy, needs a console or the emulator: docs/console31/halo31-arms.md.)
#include <cstdint>
#include <vector>
#include <GLES2/gl2.h>
namespace h31 {
struct St { int vp[4] = {0, 0, 0, 0}, sc[4] = {0, 0, 0, 0}, cm[2] = {1, 1}, sf[3] = {0, 0, 0}, so[3] = {0, 0, 0}, stencilOn = 0, scissorOn = 0, depthOn = 0, depthMask = 1, arrays = 0; };
St g_s;
std::vector<St> g_draws;
bool g_rec = false;
inline void Draw(int arrays) { if (g_rec) { St s = g_s; s.arrays = arrays; g_draws.push_back(s); } }
inline void Vp(int x, int y, int w, int h) { g_s.vp[0] = x; g_s.vp[1] = y; g_s.vp[2] = w; g_s.vp[3] = h; }
inline void Sc(int x, int y, int w, int h) { g_s.sc[0] = x; g_s.sc[1] = y; g_s.sc[2] = w; g_s.sc[3] = h; }
inline void Cap(unsigned what, int on) { if (what == 0x0B90) g_s.stencilOn = on; if (what == 0x0C11) g_s.scissorOn = on; if (what == 0x0B71) g_s.depthOn = on; }
}  // namespace h31
#undef glDrawElements
#undef glDrawArrays
#define glDrawElements(mode, count, type, indices) h31::Draw(0)
#define glDrawArrays(mode, first, count) h31::Draw(1)
#define glViewport(x, y, w, h) h31::Vp(x, y, w, h)
#define glScissor(x, y, w, h) h31::Sc(x, y, w, h)
#define glColorMask(r, g, b, a) (h31::g_s.cm[0] = (r) ? 1 : 0, h31::g_s.cm[1] = (a) ? 1 : 0)
#define glStencilFunc(f, r, m) (h31::g_s.sf[0] = static_cast<int>(f), h31::g_s.sf[1] = static_cast<int>(r), h31::g_s.sf[2] = static_cast<int>(m))
#define glStencilOp(a, b, c) (h31::g_s.so[0] = static_cast<int>(a), h31::g_s.so[1] = static_cast<int>(b), h31::g_s.so[2] = static_cast<int>(c))
#define glEnable(x) h31::Cap(x, 1)
#define glDisable(x) h31::Cap(x, 0)
#define glDepthMask(x) (h31::g_s.depthMask = (x) ? 1 : 0)
#define main kd_main
#include "keepdepth31_backend.cpp"
#undef main

int main() {
    setenv("GXV_SHADOW_STENCIL", "1", 1);
    {   // the keepdepth31 harness's own "kept" case first: it initialises the back end, registers the shaders and warms the programs
        char a0[] = "halo31", a1[] = "kept";
        char* av[] = {a0, a1, nullptr};
        if (kd_main(2, av) != 0) { std::printf("FAIL: the harness's own case\n"); return 1; }
    }
    g_fail = 0;
    uint64_t frame = 1000;
    const std::vector<Kind> kinds{kClearEfb, k3D, k3D, k2D};
    // 1. nothing armed
    WarpArm(false, 0, false);
    h31::g_draws.clear();
    h31::g_rec = true;
    Execute(Picture(frame++, kinds));
    h31::g_rec = false;
    std::vector<h31::St> plain;
    for (const h31::St& d : h31::g_draws) if (!d.arrays) plain.push_back(d);
    bool noStencil = true;
    for (const h31::St& d : plain) noStencil = noStencil && !d.stencilOn;
    Check(plain.size() == kinds.size() && noStencil, "nothing armed: one submission a draw, no stencil test");
    // 2. the same buffer as a pair's fresh buffer with two bands armed
    WarpArm(false, 0, false);
    const int band[2][4] = {{4, 2, 20, 10}, {4, 10, 20, 18}}, move[2][2] = {{-3, 1}, {2, 0}};
    g_halo.n = 2;
    for (int b = 0; b < 2; ++b) { for (int k = 0; k < 4; ++k) g_halo.rect[b][k] = band[b][k]; g_halo.move[b][0] = move[b][0]; g_halo.move[b][1] = move[b][1]; }
    g_halo.stampDraws = g_halo.freshDraws = g_halo.skipped = 0;
    g_halo.stamp = true;
    g_hrwFreshB = true;
    g_hrwFreshDebug = 0;  // (FreshBegin reads GXV_HRW_FRESH_DEBUG on a pair's first fresh picture; there is no pair here)
    h31::g_draws.clear();
    h31::g_rec = true;
    Execute(Picture(frame++, kinds));
    h31::g_rec = false;
    g_halo.stamp = false;
    g_hrwFreshB = false;
    std::vector<h31::St> got;
    for (const h31::St& d : h31::g_draws) if (!d.arrays) got.push_back(d);
    Check(got.size() == plain.size() * 3, "armed: every draw is submitted once per band and once as it is");
    std::printf("halo31 back end: plain %u submissions, armed %u; stamps counted %llu, fresh draws %llu, left alone %llu\n", static_cast<unsigned>(plain.size()), static_cast<unsigned>(got.size()),
                static_cast<unsigned long long>(g_halo.stampDraws), static_cast<unsigned long long>(g_halo.freshDraws), static_cast<unsigned long long>(g_halo.skipped));
    Check(g_halo.stampDraws == plain.size() * 2 && g_halo.freshDraws == plain.size() && g_halo.skipped == 0, "the counters agree");
    bool stampOk = got.size() == plain.size() * 3, realOk = stampOk;
    for (size_t i = 0; stampOk && i < plain.size(); ++i) {
        const h31::St& real = got[i * 3 + 2];
        const h31::St& p = plain[i];
        for (int b = 0; b < 2; ++b) {
            const h31::St& s = got[i * 3 + b];
            stampOk = stampOk && s.cm[0] == 0 && s.cm[1] == 0 && s.depthOn == 0 && s.depthMask == 0 && s.stencilOn == 1 && s.sf[0] == GL_EQUAL && s.sf[1] == 0 && s.sf[2] == 0x0F &&
                      s.so[0] == GL_KEEP && s.so[1] == GL_KEEP && s.so[2] == GL_INCR && s.scissorOn == 1 && s.sc[0] == band[b][0] && s.sc[1] == band[b][1] &&
                      s.sc[2] == band[b][2] - band[b][0] && s.sc[3] == band[b][3] - band[b][1] && s.vp[0] == real.vp[0] + move[b][0] && s.vp[1] == real.vp[1] + move[b][1] &&
                      s.vp[2] == real.vp[2] && s.vp[3] == real.vp[3];
        }
        realOk = realOk && real.stencilOn == 1 && real.sf[0] == GL_ALWAYS && real.sf[1] == 2 && real.so[0] == GL_KEEP && real.so[1] == GL_REPLACE && real.so[2] == GL_REPLACE &&
                 real.cm[0] == p.cm[0] && real.cm[1] == p.cm[1] && real.depthOn == p.depthOn && real.depthMask == p.depthMask && real.scissorOn == p.scissorOn &&
                 real.vp[0] == p.vp[0] && real.vp[1] == p.vp[1] && real.vp[2] == p.vp[2] && real.vp[3] == p.vp[3];
    }
    if (!realOk)
        for (size_t i = 0; i < plain.size() && i * 3 + 2 < got.size(); ++i) {
            const h31::St& r = got[i * 3 + 2];
            const h31::St& p = plain[i];
            std::printf("  draw %u: real st %d f %x ref %d op %x %x %x cm %d%d depth %d mask %d sc %d vp %d,%d %dx%d | plain cm %d%d depth %d mask %d sc %d vp %d,%d %dx%d\n", static_cast<unsigned>(i), r.stencilOn,
                        r.sf[0], r.sf[1], r.so[0], r.so[1], r.so[2], r.cm[0], r.cm[1], r.depthOn, r.depthMask, r.scissorOn, r.vp[0], r.vp[1], r.vp[2], r.vp[3], p.cm[0], p.cm[1], p.depthOn, p.depthMask,
                        p.scissorOn, p.vp[0], p.vp[1], p.vp[2], p.vp[3]);
        }
    Check(stampOk, "each stamp: colour and depth masked, stencil EQUAL 0 (low nibble) -> INCR, the band's scissor, the viewport moved by the band's vector");
    Check(realOk, "the draw itself: its own viewport, scissor, colour mask and depth state, stencil ALWAYS 2 with REPLACE on pass and on depth fail");
    Check(h31::g_s.stencilOn == 0, "the stencil test is off after the buffer");
    // 3. the next ordinary buffer: nothing left over
    WarpArm(false, 0, false);
    h31::g_draws.clear();
    h31::g_rec = true;
    Execute(Picture(frame++, kinds));
    h31::g_rec = false;
    size_t after = 0;
    bool afterPlain = true;
    for (const h31::St& d : h31::g_draws) if (!d.arrays) { ++after; afterPlain = afterPlain && !d.stencilOn; }
    Check(after == plain.size() && afterPlain, "the buffer after it: one submission a draw again, no stencil test");
    // 4. the scene pass's reset and fill
    {
        warp31::State& W = warp31::g_w;
        W.halo = port::halo31::Plan{};
        W.halo.on = true;
        W.halo.n = 2;
        W.halo.rect[0] = 4; W.halo.rect[1] = 2; W.halo.rect[2] = 20; W.halo.rect[3] = 18;
        for (int b = 0; b < 2; ++b) {
            for (int k = 0; k < 4; ++k) W.halo.band[b].rect[k] = band[b][k];
            W.halo.band[b].fill[0] = -move[b][0] * 2; W.halo.band[b].fill[1] = -move[b][1] * 2;
        }
        W.pl36.on = false; W.d41 = false; W.haveDepth = false;
        const uint64_t warped = W.st.warped;
        h31::g_draws.clear();
        h31::g_rec = true;
        warp31::DrawWarp(-1.0f, false, 3);
        h31::g_rec = false;
        Check(h31::g_draws.size() == 1 && h31::g_draws[0].arrays == 1 && h31::g_draws[0].cm[0] == 0 && h31::g_draws[0].cm[1] == 0 && h31::g_draws[0].stencilOn == 1 &&
              h31::g_draws[0].sf[0] == GL_ALWAYS && h31::g_draws[0].sf[1] == 0 && h31::g_draws[0].so[2] == GL_REPLACE && h31::g_draws[0].scissorOn == 1 && h31::g_draws[0].sc[0] == 4 &&
              h31::g_draws[0].sc[1] == 2 && h31::g_draws[0].sc[2] == 16 && h31::g_draws[0].sc[3] == 16,
              "the reset: one colour-masked quad of the scene pass in the plan's rectangle, stencil ALWAYS 0 REPLACE");
        h31::g_draws.clear();
        h31::g_rec = true;
        warp31::DrawWarp(-1.0f, false, 4);
        h31::g_rec = false;
        bool fillOk = h31::g_draws.size() == 2;
        for (int b = 0; fillOk && b < 2; ++b) {
            const h31::St& s = h31::g_draws[static_cast<size_t>(b)];
            fillOk = s.arrays == 1 && s.cm[0] == 1 && s.stencilOn == 1 && s.sf[0] == GL_EQUAL && s.sf[1] == 1 && s.sf[2] == 0x0F && s.so[0] == GL_KEEP && s.so[1] == GL_KEEP && s.so[2] == GL_KEEP &&
                     s.scissorOn == 1 && s.sc[0] == band[b][0] && s.sc[1] == band[b][1] && s.vp[0] == -move[b][0] * 2 && s.vp[1] == -move[b][1] * 2 && s.depthOn == 0;
        }
        Check(fillOk, "the fill: the scene pass once per band, colour on, where the stencil is 1, the band's scissor, the viewport moved by the band's fill vector");
        Check(h31::g_s.stencilOn == 0 && h31::g_s.scissorOn == 0 && h31::g_s.cm[0] == 1 && W.st.warped == warped, "after the fill: stencil and scissor off, colour on, nothing counted as a warp");
    }
    std::printf(g_fail ? "halo31 back end: FAIL (%d)\n" : "halo31 back end: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}

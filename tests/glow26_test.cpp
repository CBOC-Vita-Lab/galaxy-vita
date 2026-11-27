// glow26 host test: the pure logic of port/glow26.h (profile, classification with hysteresis, intensity compensation, rectangles, metrics) and a CPU model of the look-alike
// (reduced-size glow with a depth-aware composite) against the full-size picture: the numbers GXV_GLOW26_VERIFY reports on the console, here on a synthetic scene.
// Build: tools/test_glow26.sh
#include "port/glow26.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
using namespace port::glow26;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---- the synthetic picture: 480 x 272, a smooth background gradient, a near box (depth 0.3), glow of `layers` stacked soft discs at depth 0.5 (far background 1.0)
struct Scene { int w = 480, h = 272; int layers = 4; float sigma = 40.0f, amp = 0.12f; };
static float GlowAt(const Scene& s, float x, float y) {
    float v = 0;
    for (int l = 0; l < s.layers; ++l) {
        const float cx = s.w * 0.5f + (l - s.layers * 0.5f) * 6.0f, cy = s.h * 0.5f + (l & 1 ? 5.0f : -5.0f);
        const float dx = x - cx, dy = y - cy;
        v += s.amp * std::exp(-(dx * dx + dy * dy) / (2.0f * s.sigma * s.sigma));
    }
    return v;
}
static float DepthAt(const Scene& s, float x, float y) { // a near box right of the centre and a pole: depth edges through the glow
    if (x > s.w * 0.55f && x < s.w * 0.70f && y > s.h * 0.30f && y < s.h * 0.90f) return 0.3f;
    if (x > s.w * 0.38f && x < s.w * 0.40f) return 0.4f;
    return 1.0f;
}
static uint8_t Q(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
static void Base(const Scene& s, std::vector<float>& base) {
    base.assign(static_cast<size_t>(s.w) * s.h, 0);
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) base[static_cast<size_t>(y) * s.w + x] = DepthAt(s, x + 0.5f, y + 0.5f) < 0.5f ? 0.55f : 0.15f + 0.2f * x / s.w;
}
static void Full(const Scene& s, const std::vector<float>& base, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(s.w) * s.h * 4, 255);
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        const size_t i = static_cast<size_t>(y) * s.w + x;
        float v = base[i];
        if (DepthAt(s, x + 0.5f, y + 0.5f) > 0.5f) v += GlowAt(s, x + 0.5f, y + 0.5f);
        for (int c = 0; c < 3; ++c) out[i * 4 + c] = Q(v * (c == 0 ? 1.0f : c == 1 ? 0.9f : 0.7f));
    }
}
// glow drawn into a target of 1/div size (depth-tested against the low-res depth copy: the depth at the texel centre), composited with the nearest-depth rule (level 1) or plain bilinear (level 0)
static void Cheap(const Scene& s, const std::vector<float>& base, int div, int level, int collapse, std::vector<uint8_t>& out) {
    const int lw = s.w / div, lh = s.h / div;
    std::vector<float> lg(static_cast<size_t>(lw) * lh), lz(static_cast<size_t>(lw) * lh);
    Scene one = s; // collapse: `collapse` layers drawn as one layer with the summed intensity (here: sample the sum once; same value, fewer evaluations; the test checks the light)
    for (int y = 0; y < lh; ++y) for (int x = 0; x < lw; ++x) {
        const float fx = (x + 0.5f) * div, fy = (y + 0.5f) * div;
        lz[static_cast<size_t>(y) * lw + x] = DepthAt(s, fx, fy);
        lg[static_cast<size_t>(y) * lw + x] = lz[static_cast<size_t>(y) * lw + x] > 0.5f ? GlowAt(one, fx, fy) : 0.0f;
    }
    (void)collapse;
    out.assign(static_cast<size_t>(s.w) * s.h * 4, 255);
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        const float gx = (x + 0.5f) / div - 0.5f, gy = (y + 0.5f) / div - 0.5f;
        const int x0 = static_cast<int>(std::floor(gx)), y0 = static_cast<int>(std::floor(gy));
        const float fx = gx - x0, fy = gy - y0;
        const float zf = DepthAt(s, x + 0.5f, y + 0.5f);
        double acc = 0, wsum = 0, nearestD = 1e9, nearestV = 0;
        for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
            const int tx = std::clamp(x0 + i, 0, lw - 1), ty = std::clamp(y0 + j, 0, lh - 1);
            const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const float zt = lz[static_cast<size_t>(ty) * lw + tx], gv = lg[static_cast<size_t>(ty) * lw + tx];
            const double dd = std::fabs(zt - zf);
            if (dd < nearestD) { nearestD = dd; nearestV = gv; }
            if (level == 0 || dd <= 0.04 * zf) { acc += wgt * gv; wsum += wgt; }
        }
        float g = level == 0 ? static_cast<float>(acc) : (wsum > 1e-6 ? static_cast<float>(acc / wsum) : static_cast<float>(nearestV));
        if (level == 0 && zf < 0.5f) g = 0; // plain composite: the picture's own depth test at full size is not available; the low-res target only (what level 0 does: glow leaks over the near box edge by <= div px)
        const size_t k = static_cast<size_t>(y) * s.w + x;
        float v = base[k] + (zf > 0.5f ? g : 0.0f);
        for (int c = 0; c < 3; ++c) out[k * 4 + c] = Q(v * (c == 0 ? 1.0f : c == 1 ? 0.9f : 0.7f));
    }
}

int main() {
    // ---- profile
    {
        Profile p0; Profile p1 = Apply(1, p0), p2 = Apply(2, p0);
        CHECK(Apply(0, p0).autoMode == 0 && Apply(0, p0).indFlat == 0);
        CHECK(p1.autoMode == 1 && p1.ups == 1 && p1.defer == 1 && p1.autoMilli == 500 && p1.indFlat == 1 && p1.bloomFold == 1 && p1.lightvol == 3);
        CHECK(p2.defer == 2 && p2.autoMilli == 300 && p2.bloomFold == 2 && p2.lightvol == 7 && p2.quarterScreens < 3.0f && p2.softFrac < 0.85f);
        Profile c = p0; c.autoMode = 2;
        CHECK(Apply(2, c).autoMode == 2);                         // a census stays a census
        Profile hi = p0; hi.indFlat = 2; hi.bloomFold = 2; hi.autoMilli = 100;
        CHECK(Apply(1, hi).indFlat == 2 && Apply(1, hi).bloomFold == 2 && Apply(1, hi).autoMilli == 100); // never lowers
        CHECK(IndFlatFloor(0, 0) == 0 && IndFlatFloor(1, 0) == 1 && IndFlatFloor(2, 2) == 2);
        CHECK(BloomFoldFloor(2, 0) == 2 && BloomFoldFloor(1, 0) == 1 && BloomFoldFloor(0, 0) == 0);
        CHECK(LightVolFloor(2, 0) == 7 && LightVolFloor(1, 8) == 11);
        CHECK(ClampLevel(-3) == 0 && ClampLevel(1) == 1 && ClampLevel(9) == 2);
    }
    // ---- classification
    {
        ClassCfg cc; cc.exitFactor = 0.95f; LayerObs soft; soft.blend = true; soft.additive = true; soft.fillScreens = 0.9f; soft.coverScreens = 0.5f; soft.widthPx = 600; soft.soft = 0.95f;
        CHECK(Qualifies(soft, cc, false));
        LayerObs o = soft; o.depthWrite = true; CHECK(!Qualifies(o, cc, false));
        o = soft; o.alphaTest = true; CHECK(!Qualifies(o, cc, false));
        o = soft; o.hud = true; CHECK(!Qualifies(o, cc, false));
        o = soft; o.mario = true; CHECK(!Qualifies(o, cc, false));
        o = soft; o.opaque = true; o.blend = false; CHECK(!Qualifies(o, cc, false));
        o = soft; o.near_ = true; CHECK(!Qualifies(o, cc, false));
        o = soft; o.widthPx = 8; CHECK(!Qualifies(o, cc, false));          // star bit / coin / sparkle
        o = soft; o.soft = 0.3f; CHECK(!Qualifies(o, cc, false));          // crisp texture
        o = soft; o.fillScreens = 0.2f; o.coverScreens = 0.25f; CHECK(!Qualifies(o, cc, false)); // small and not stacked
        o = soft; o.fillScreens = 0.4f; o.coverScreens = 0.1f; CHECK(Qualifies(o, cc, false));   // small but stacked 4x
        // hysteresis: fill hovering around the limit with noise never flips more than twice in 2000 pictures
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.0f, 0.06f);
        KeyState ks; LayerObs h = soft; uint32_t flips = 0, flipsNoHyst = 0; bool prev = false;
        ClassCfg nohy = cc; nohy.enterN = 1; nohy.exitN = 1; nohy.exitFactor = 1.0f; KeyState k2; bool prev2 = false;
        for (int i = 0; i < 2000; ++i) {
            h.fillScreens = 0.5f + nd(rng); h.coverScreens = 1.0f;
            const bool q = Qualifies(h, cc, ks.soft);
            const bool s = Step(ks, q, cc); if (s != prev) { ++flips; prev = s; }
            const bool s2 = Step(k2, Qualifies(h, nohy, k2.soft), nohy); if (s2 != prev2) { ++flipsNoHyst; prev2 = s2; }
        }
        std::printf("hysteresis: %u flips with enter %u / exit %u, %u without\n", flips, cc.enterN, cc.exitN, flipsNoHyst);
        CHECK(flips <= 6 && flips * 4 < flipsNoHyst + 4);
        // a clear signal flips after exactly enterN pictures, and back after exitN
        KeyState a; ClassCfg c3 = cc; int at = -1; for (int i = 1; i <= 10; ++i) if (Step(a, true, c3) && at < 0) at = i; CHECK(at == static_cast<int>(c3.enterN));
        at = -1; for (int i = 1; i <= 20; ++i) if (!Step(a, false, c3) && at < 0) at = i; CHECK(at == static_cast<int>(c3.exitN));
    }
    // ---- intensity compensation
    {
        CHECK(CollapseScale(4) == 4.0f && StrideScale(0) == 1.0f);
        CHECK(std::fabs(KeptLight(0.0f, 0.05f, 4, 4.0f) - 1.0f) < 1e-5f);            // under the clamp: exact
        CHECK(std::fabs(KeptLight(0.5f, 0.05f, 4, 4.0f) - 1.0f) < 1e-5f);
        CHECK(KeptLight(0.9f, 0.05f, 4, 4.0f) <= 1.0f + 1e-5f && std::fabs(KeptLight(0.9f, 0.05f, 4, 4.0f) - 1.0f) < 1e-5f); // both clamp at 1
        CHECK(KeptLight(0.8f, 0.1f, 4, 1.0f) < 0.6f);                                // scale too small: the light shrinks
        CHECK(QuantErrLevels(4) == 2.0f);
    }
    // ---- rectangles
    {
        std::vector<float> t(64 * 64, 0.0f);
        for (int y = 20; y < 40; ++y) for (int x = 10; x < 50; ++x) t[y * 64 + x] = 0.5f;
        Rect e = OpaqueExtent(t.data(), 64, 64, 1.0f / 255.0f);
        CHECK(e.x0 > 0.13f && e.x0 < 0.16f && e.x1 > 0.78f && e.x1 < 0.82f && e.y0 > 0.29f && e.y1 < 0.65f);
        std::vector<float> z(16, 0.0f); CHECK(Area(OpaqueExtent(z.data(), 4, 4, 0.01f)) == 0.0f);
        Rect quad{-3, -3, 3, 3}, screen{-1, -1, 1, 1}, full{0, 0, 1, 1};
        TightQuad all = Tighten(quad, full, screen);
        CHECK(all.quad.x0 == -1 && all.quad.x1 == 1 && std::fabs(all.u0 - 1.0f / 3.0f) < 1e-5f && std::fabs(all.u1 - 2.0f / 3.0f) < 1e-5f); // screen cap alone: uv follows
        TightQuad tt = Tighten(Rect{-1, -1, 1, 1}, e, screen);
        CHECK(Area(tt.quad) < 0.3f * Area(Rect{-1, -1, 1, 1}) && tt.u0 >= e.x0 - 1e-6f);
        CHECK(FillSavedScreens(Rect{-1, -1, 1, 1}, tt.quad, 4.0f, 3) > 1.0f);
        CHECK(PixelsSaved(3.86f, 2, 0.5f) > 0 && PixelsSaved(0.3f, 2, 0.5f) < 0);   // a small layer does not pay for its pass
    }
    // ---- metrics
    {
        std::vector<uint8_t> a(100 * 4, 100), b(100 * 4, 110);
        Diff d; Accum(d, a.data(), b.data(), 100);
        CHECK(d.n == 100 && std::fabs(Mean(d) - 10.0) < 1e-9 && d.maxAbs == 10 && std::fabs(LightRatio(d) - 1.1) < 1e-9 && std::fabs(Psnr(d) - 10.0 * std::log10(65025.0 / 100.0)) < 1e-6);
        CHECK(std::fabs(LayerRatio(300, 270, 100) - 0.85) < 1e-9 && LayerRatio(100, 100, 100) == 1.0);
        CHECK(!Judge(1.12, 0.1, 0.0).lightOk && Judge(1.05, 0.5, 0.0).lightOk && Judge(1.0, 2.0, 0.6).meanOk && !Judge(1.0, 3.0, 0.6).meanOk);
        CHECK(PhaseOf(0, 60, 2) == P_ORIG && PhaseOf(60, 60, 2) == P_CHEAP && PhaseOf(120, 60, 2) == P_ORIG && PhaseOf(180, 60, 2) == P_OFF && PhaseOf(180, 60, 1) == P_CHEAP && PhaseOf(240, 60, 2) == P_ORIG);
        CHECK(BlockStart(120, 60) && !BlockStart(121, 60) && LastOfBlock(59, 60) && !LastOfBlock(60, 60) && IsCheapBlock(61, 60) && !IsCheapBlock(125, 60));
        Census c; c.pictures = 2; c.compLayers = 6; c.compKeys = 2; c.div = 2; c.fillMovedScreens = 2 * 4.79; c.rectScreens = 2 * 0.5; c.indFlat = 1;
        char buf[700]; Format(buf, sizeof buf, "IceVolcanoGalaxy", 1, c);
        std::printf("%s\n", buf);
        CHECK(std::strstr(buf, "compositor 3.00") && std::strstr(buf, "GXV_IND_FLAT=1") && std::strstr(buf, "fill saved 1"));
    }
    // ---- the look-alike on a synthetic picture (what the verify run measures on the console)
    {
        Scene s; std::vector<float> base; Base(s, base); std::vector<uint8_t> full;
        Full(s, base, full);
        struct Case { int div, level; const char* name; double maxMean, maxLightDev; } cases[] = {
            {2, 1, "half size, nearest-depth composite", 0.6, 0.02}, {4, 1, "quarter size, nearest-depth composite", 1.0, 0.03}, {2, 0, "half size, plain composite", 1.5, 0.05}};
        std::vector<uint8_t> out;
        for (const Case& c : cases) {
            Cheap(s, base, c.div, c.level, s.layers, out);
            Diff d; Accum(d, full.data(), out.data(), static_cast<size_t>(s.w) * s.h);
            // the glow's own light: baseline = the picture without glow
            Scene none = s; none.amp = 0; std::vector<uint8_t> nb; Full(none, base, nb);
            Diff l0, l1; Accum(l0, nb.data(), full.data(), static_cast<size_t>(s.w) * s.h); Accum(l1, nb.data(), out.data(), static_cast<size_t>(s.w) * s.h);
            const double ratio = LayerRatio(l0.lightB, l1.lightB, l0.lightA);
            std::printf("%-40s mean %.3f max %u PSNR %.1f dB light ratio %.4f layer ratio %.4f\n", c.name, Mean(d), d.maxAbs, Psnr(d), LightRatio(d), ratio);
            CHECK(Mean(d) <= c.maxMean);
            CHECK(std::fabs(ratio - 1.0) <= c.maxLightDev + 0.02);
            if (c.level == 1) CHECK(d.maxAbs <= 40);
        }
        // honest limit: a small, sharp glow (sigma 1.5 px) at quarter size is NOT a good candidate; the classifier's width and softness rules keep it at full size
        {
            Scene sm = s; sm.sigma = 1.5f; std::vector<float> b2; Base(sm, b2); std::vector<uint8_t> f2, o2;
            Full(sm, b2, f2); Cheap(sm, b2, 4, 1, sm.layers, o2);
            Diff d2; Accum(d2, f2.data(), o2.data(), static_cast<size_t>(sm.w) * sm.h);
            std::printf("%-40s mean %.3f max %u PSNR %.1f dB (small sharp glow: why SoftScore / minWidthPx gate it)\n", "quarter size, sigma 1.5 px", Mean(d2), d2.maxAbs, Psnr(d2));
        }
        // collapse: 4 stacked layers at amp a == 1 layer at 4a (sum kept) away from the clamp
        Scene four = s, one = s; one.layers = 1; one.amp = s.amp * 4.0f;
        double sum4 = 0, sum1 = 0;
        for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) { sum4 += GlowAt(four, x + 0.5f, y + 0.5f); sum1 += GlowAt(one, x + 0.5f, y + 0.5f); }
        std::printf("collapse 4 -> 1 at x4: light ratio %.4f (the layer offsets differ by 6 px)\n", sum1 / sum4);
        CHECK(std::fabs(sum1 / sum4 - 1.0) < 0.02);
    }
    std::printf("glow26_test: %d checks, %d failures\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}

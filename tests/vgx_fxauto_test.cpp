// compositor25: host test of the automatic choice model (platform/vgx/src/vgx_fxauto.h): no GL, no game. Freezeflame- and Dreadnought-shaped
// pictures, the budget rule, hysteresis and one-way decisions under noisy fill, the divisor chosen once, run formation and the census of
// runs if draws between them could be hoisted. Exit status 0 when every check passes.
#include "vgx_fxauto.h"

#include <cstdio>
#include <cstdlib>

using namespace vgx::fxauto;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static Item Rt(uint32_t key, float fill, float ms, uint8_t flags = IF_RECT, float x0 = -1, float y0 = -1, float x1 = 1, float y1 = 1) {
    Item i; i.kind = IK_ROUTABLE; i.key = key; i.fill = fill; i.ms = ms; i.flags = flags; i.x0 = x0; i.y0 = y0; i.x1 = x1; i.y1 = y1; return i;
}
static Item Br(uint8_t flags = 0, float x0 = -1, float y0 = -1, float x1 = 1, float y1 = 1) {
    Item i; i.kind = IK_BREAK; i.flags = flags; i.x0 = x0; i.y0 = y0; i.x1 = x1; i.y1 = y1; return i;
}
static Item Bd() { Item i; i.kind = IK_BOUNDARY; return i; }
static uint32_t g_seed = 99;
static float Rnd() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<float>((g_seed >> 8) & 0xFFFF) / 65535.0f; }

// Freezeflame, from the c72 dump (picture 6241): scene 1 has fh 2763ac (0.93 screens, dependent, mid scene), scene 3 starts with a depth-only
// quad and then fh d97a8b (3.86 screens, 1 texture) and fh 443366 (0.93, dependent).
static std::vector<Item> Freeze(float jitter) {
    std::vector<Item> v;
    v.push_back(Br());                                              // scene 1: the silhouette fill
    for (int i = 0; i < 7; ++i) v.push_back(Br());
    v.push_back(Rt(0x2763ac, 0.93f * (1 + jitter * (Rnd() - 0.5f)), 3.0f));
    for (int i = 0; i < 12; ++i) v.push_back(Br());
    v.push_back(Bd());                                              // the Indirect copy
    v.push_back(Br());                                              // scene 3: the depth-only quad
    v.push_back(Rt(0xd97a8b, 3.86f * (1 + jitter * (Rnd() - 0.5f)), 1.5f));
    v.push_back(Rt(0x443366, 0.93f * (1 + jitter * (Rnd() - 0.5f)), 3.0f));
    for (int i = 0; i < 20; ++i) v.push_back(Br(i % 3 ? IF_DEPTHW : 0));
    return v;
}
static std::vector<Item> Dread() {
    std::vector<Item> v;
    for (int i = 0; i < 40; ++i) v.push_back(Br());
    for (int i = 0; i < 6; ++i) v.push_back(Rt(0xc951dd, 0.08f, 1.5f)); // 0.52 screens in all
    for (int i = 0; i < 60; ++i) v.push_back(Rt(0x450280, 0.001f, 1.5f));
    return v;
}

int main() {
    // ---- 1. Freezeflame: the glow run (d97a8b + 443366) is routed, the lone dependent layer in scene 1 is not (the budget rule)
    {
        Model m;
        for (int i = 0; i < 200; ++i) m.Observe(Freeze(0.1f));
        CHECK(m.Routed(0xd97a8b));
        CHECK(m.Routed(0x443366));
        CHECK(!m.Routed(0x2763ac));
        CHECK(m.Div() == 2); // 4.79 screens, 3.86 of them soft at best (81 %): half
        Decision d[16];
        const int n = m.TakeDecisions(d, 16);
        CHECK(n == 2);
        // the run as the back end forms it with the decided set
        std::vector<Run> runs;
        FormRuns(Freeze(0.0f), [&](const Item& x) { return m.Routed(x.key); }, runs);
        CHECK(runs.size() == 1);
        if (!runs.empty()) {
            const Run& r = runs[0];
            CHECK(r.draws == 2 && r.nKeys == 2 && !r.head && !r.shareDepth);
            CHECK(ScenesAdded(r) == 2);
            CHECK(r.fill > 4.7f && r.fill < 4.9f);
            const float save = RunSaveMs(r, 2), cost = RunCostMs(r, 2, m.cfg);
            std::printf("freeze run: fill %.2f screens, save %.2f ms, cost %.2f ms, net %.2f ms, %d scenes\n", r.fill, save, cost, save - cost, ScenesAdded(r));
            CHECK(save - cost > 3.5f && save - cost < 5.5f);
        }
    }
    // ---- 2. the same lone layer at the head of a scene costs one scene less and pays (0.93 x 3.0 ms x 0.75 = 2.09 against 0.6 + 0.26 + 0.68)
    {
        Model m;
        for (int i = 0; i < 100; ++i) {
            std::vector<Item> v;
            v.push_back(Bd());
            v.push_back(Rt(0x6a2763, 0.93f, 3.0f));
            v.push_back(Br());
            m.Observe(v);
        }
        CHECK(m.Routed(0x6a2763));
        std::vector<Run> runs;
        std::vector<Item> v = {Bd(), Rt(0x6a2763, 0.93f, 3.0f), Br()};
        FormRuns(v, [&](const Item& x) { return m.Routed(x.key); }, runs);
        CHECK(runs.size() == 1 && runs[0].head && ScenesAdded(runs[0]) == 1);
    }
    // ---- 3. Dreadnought: 0.52 screens of additive fill in six small draws, 60 tiny ones: nothing is routed
    {
        Model m;
        for (int i = 0; i < 200; ++i) m.Observe(Dread());
        CHECK(m.RoutedKeys() == 0);
        Decision d[16];
        CHECK(m.TakeDecisions(d, 16) == 0);
    }
    // ---- 4. thin and near draws do not count and keep a key at full size
    {
        Model m;
        for (int i = 0; i < 200; ++i) {
            std::vector<Item> v = {Br(), Rt(0x111111, 2.0f, 1.5f, IF_RECT | IF_THIN), Rt(0x222222, 2.0f, 1.5f, IF_RECT | IF_NEAR), Rt(0x333333, 4.0f, 1.5f)};
            m.Observe(v);
        }
        CHECK(!m.Routed(0x111111) && !m.Routed(0x222222) && m.Routed(0x333333));
    }
    // ---- 5. stability: noisy fill around the threshold. A key is routed at most once and never un-routed (no flip-flop), whatever the noise
    {
        int flips = 0, routedKeys = 0;
        for (int trial = 0; trial < 200; ++trial) {
            Model m;
            bool was[3] = {false, false, false};
            const float base[3] = {0.5f + 0.5f * Rnd(), 0.75f, 1.2f};
            for (int p = 0; p < 600; ++p) {
                std::vector<Item> v = {Bd(), Rt(1, base[0] * (0.6f + 0.8f * Rnd()), 3.0f), Br(), Rt(2, base[1] * (0.5f + Rnd()), 3.0f), Br(), Rt(3, base[2] * (0.5f + Rnd()), 3.0f)};
                if (Rnd() < 0.3f) v.erase(v.begin() + 1);
                m.Observe(v);
                const bool now[3] = {m.Routed(1), m.Routed(2), m.Routed(3)};
                for (int k = 0; k < 3; ++k) {
                    if (was[k] && !now[k]) ++flips;
                    was[k] = now[k];
                }
            }
            for (int k = 0; k < 3; ++k) routedKeys += was[k] ? 1 : 0;
            Decision d[16];
            CHECK(m.TakeDecisions(d, 16) <= 3); // (each key logged at most once)
        }
        std::printf("stability: 200 noisy trials of 600 pictures: %d un-routings (must be 0), %d keys routed in the end\n", flips, routedKeys);
        CHECK(flips == 0);
    }
    // ---- 6. a decision needs `confirm` consecutive windows: one burst of big fill is not taken
    {
        Model m;
        for (int p = 0; p < 16; ++p) m.Observe({Bd(), Rt(7, 3.0f, 1.5f)});            // one qualifying window
        CHECK(!m.Routed(7));
        for (int p = 0; p < 16; ++p) m.Observe({Bd(), Rt(7, 0.1f, 1.5f)});            // then a quiet one: the streak is broken
        for (int p = 0; p < 16; ++p) m.Observe({Bd(), Rt(7, 3.0f, 1.5f)});
        CHECK(!m.Routed(7));
        for (int p = 0; p < 16; ++p) m.Observe({Bd(), Rt(7, 3.0f, 1.5f)});
        CHECK(m.Routed(7));
    }
    // ---- 7. the divisor is chosen with the first decision and never changes: a later 12-screen key does not move it to a quarter
    {
        Model m;
        for (int p = 0; p < 64; ++p) m.Observe({Bd(), Rt(1, 3.0f, 1.5f)});
        CHECK(m.Routed(1) && m.Div() == 2);
        for (int p = 0; p < 64; ++p) m.Observe({Bd(), Rt(1, 3.0f, 1.5f), Br(), Rt(2, 12.0f, 1.5f)});
        CHECK(m.Routed(2) && m.Div() == 2);
        Model q;
        for (int p = 0; p < 64; ++p) q.Observe({Bd(), Rt(9, 9.0f, 1.5f, IF_RECT | IF_SOFT)});
        CHECK(q.Routed(9) && q.Div() == 4);
        q.Reset();
        CHECK(q.RoutedKeys() == 0);
        Cfg c; c.div = 2;
        CHECK(ChooseDiv(20.0f, 20.0f, c) == 2);
    }
    // ---- 8. runs, scenes, shared depth and the hoist census
    {
        auto all = [](const Item&) { return true; };
        std::vector<Run> runs, tmp;
        // two runs separated by an opaque draw that writes depth: two runs, the second cannot share the depth copy
        std::vector<Item> a = {Bd(), Rt(1, 1, 1.5f), Br(IF_DEPTHW | IF_RECT), Rt(1, 1, 1.5f)};
        CHECK(FormRuns(a, all, runs) == 2);
        CHECK(runs[0].head && !runs[1].head && !runs[1].shareDepth);
        CHECK(ScenesAdded(runs[0]) + ScenesAdded(runs[1]) == 3);
        // separated by a HUD-like draw that does not write depth: the second shares the first's depth copy
        std::vector<Item> b = {Bd(), Rt(1, 1, 1.5f), Br(IF_RECT, 0.5f, 0.5f, 0.9f, 0.9f), Rt(1, 1, 1.5f, IF_RECT, -1, -1, 0.0f, 0.0f)};
        FormRuns(b, all, runs);
        CHECK(runs.size() == 2 && runs[1].shareDepth);
        CHECK(RunCostMs(runs[1], 2, Cfg{}) < RunCostMs(runs[1], 2, [] { Cfg c; return c; }()) + 1e-6f);
        // hoisting: the between draw is disjoint from both runs -> merges; additive -> merges; opaque overlapping -> does not
        std::vector<Item> c = {Rt(1, 1, 1.5f, IF_RECT, -1, -1, 0, 0), Br(IF_RECT, 0.5f, 0.5f, 1, 1), Rt(1, 1, 1.5f, IF_RECT, -1, -1, 0, 0)};
        CHECK(RunsIfHoisted(c, all, tmp) == 1);
        std::vector<Item> d = {Rt(1, 1, 1.5f), Br(IF_ADD), Rt(1, 1, 1.5f)};
        CHECK(RunsIfHoisted(d, all, tmp) == 1);
        std::vector<Item> e = {Rt(1, 1, 1.5f), Br(IF_RECT | IF_DEPTHW), Rt(1, 1, 1.5f)};
        CHECK(RunsIfHoisted(e, all, tmp) == 2);
        std::vector<Item> f = {Rt(1, 1, 1.5f), Br(), Rt(1, 1, 1.5f)}; // unknown rectangle: not moved
        CHECK(RunsIfHoisted(f, all, tmp) == 2);
        std::vector<Item> g = {Rt(1, 1, 1.5f), Bd(), Rt(1, 1, 1.5f)}; // a scene boundary: never
        CHECK(RunsIfHoisted(g, all, tmp) == 2);
    }
    // ---- 9. thin test
    CHECK(IsThin(3.0f, 2, 2.0f) && !IsThin(5.0f, 2, 2.0f) && IsThin(7.0f, 4, 2.0f) && !IsThin(1.0f, 2, 0.0f));
    // ---- 10. round 2: the size from the softness (>= 90 % of the fill soft and >= 4 screens: quarter; otherwise half)
    {
        Cfg c;
        CHECK(ChooseDiv(5.0f, 5.0f, c) == 4);
        CHECK(ChooseDiv(5.0f, 4.4f, c) == 2);   // 88 %
        CHECK(ChooseDiv(5.0f, 4.5f, c) == 4);   // 90 %
        CHECK(ChooseDiv(3.9f, 3.9f, c) == 2);   // under 4 screens
        CHECK(ChooseDiv(12.0f, 0.0f, c) == 2);  // a lot of fill that is not soft (the old 8-screen rule would have said quarter)
        Cfg f4; f4.div = 4;
        CHECK(ChooseDiv(1.0f, 0.0f, f4) == 4);
        // Freezeflame's run: 3.86 soft + 0.93 dependent = 81 % soft: half; the sheets alone would be quarter
        Model m;
        auto pic = [](bool dep) {
            std::vector<Item> v; v.push_back(Bd()); v.push_back(Br());
            v.push_back(Rt(0xd97a8b, 3.86f, 1.5f, IF_RECT | IF_SOFT));
            v.push_back(Rt(0x443366, 0.93f, 3.0f, IF_RECT | (dep ? IF_DEP : 0)));
            return v; };
        for (int i = 0; i < 64; ++i) m.Observe(pic(true));
        CHECK(m.Routed(0xd97a8b) && m.Routed(0x443366) && m.Div() == 2);
        Model q;
        for (int i = 0; i < 64; ++i) { auto v = pic(false); v.pop_back(); v.push_back(Rt(0x443366, 0.2f, 1.5f, IF_RECT | IF_SOFT)); v[2].fill = 4.4f; q.Observe(v); }
        CHECK(q.Routed(0xd97a8b) && q.Div() == 4);
    }
    // ---- 11. round 2: deferred runs. Three layers of one stretch separated by depth writers: three runs in place, one deferred
    {
        auto all = [](const Item&) { return true; };
        std::vector<Run> runs;
        auto mk = [](uint16_t seg, uint8_t extra) { Item i = Rt(1, 1.0f, 1.5f, IF_RECT | extra); i.seg = seg; return i; };
        std::vector<Item> v = {Bd(), Br(IF_DEPTHW), mk(0, IF_DEFER), Br(IF_DEPTHW | IF_RECT), mk(0, IF_DEFER), Br(IF_DEPTHW | IF_RECT), mk(0, IF_DEFER), Br(IF_DEPTHW | IF_RECT)};
        CHECK(FormRuns(v, all, runs, false) == 3);
        CHECK(FormRuns(v, all, runs, true) == 1);
        CHECK(runs[0].deferred && runs[0].draws == 3 && !runs[0].head && !runs[0].shareDepth && ScenesAdded(runs[0]) == 2);
        // two stretches (a barrier between): one deferred run in each
        std::vector<Item> w = {Br(), mk(0, IF_DEFER), Br(IF_DEPTHW), mk(0, IF_DEFER), Bd(), Br(), mk(1, IF_DEFER), Br(IF_DEPTHW), mk(1, IF_DEFER)};
        CHECK(FormRuns(w, all, runs, true) == 2);
        // an item the back end could not prove stays in place and ends no deferred run
        std::vector<Item> x = {Br(), mk(0, IF_DEFER), Br(IF_DEPTHW), mk(0, 0), mk(0, 0), Br(IF_DEPTHW), mk(0, IF_DEFER)};
        CHECK(FormRuns(x, all, runs, true) == 2);
        // the budget: three 1.0-screen direct layers (1.5 ms a screen) are worth 0.75 x 1.5 = 1.1 ms each at half size; a run costs about 2.0 ms
        // alone, so in place (three runs) none of them pays; deferred into one run all three do (3.4 ms saved for 2.0 spent)
        auto pic = [&](bool deferOk) {
            std::vector<Item> p = {Bd(), Br(IF_DEPTHW)};
            for (int i = 0; i < 3; ++i) { p.push_back(mk(0, deferOk ? IF_DEFER : 0)); p.push_back(Br(IF_DEPTHW | IF_RECT)); }
            return p; };
        Model off, on;
        on.cfg.defer = true;
        for (int i = 0; i < 128; ++i) { off.Observe(pic(true)); on.Observe(pic(true)); }
        CHECK(off.RoutedKeys() == 0);
        CHECK(on.RoutedKeys() == 1);
        Model noProof; noProof.cfg.defer = true;
        for (int i = 0; i < 128; ++i) noProof.Observe(pic(false));
        CHECK(noProof.RoutedKeys() == 0);
    }
    // ---- 12. round 2: a key joins a run that another key pays for when its own saving is positive; alone it would not
    {
        auto mk = [](uint32_t key, float fill, uint8_t extra) { Item i = Rt(key, fill, 1.5f, IF_RECT | extra); i.seg = 0; return i; };
        auto pic = [&](bool withBig) {
            std::vector<Item> p = {Bd(), Br(IF_DEPTHW)};
            if (withBig) p.push_back(mk(1, 3.0f, IF_DEFER));
            p.push_back(Br(IF_DEPTHW | IF_RECT));
            p.push_back(mk(2, 0.45f, IF_DEFER));
            p.push_back(Br(IF_DEPTHW | IF_RECT));
            return p; };
        Model alone; alone.cfg.defer = true;
        for (int i = 0; i < 128; ++i) alone.Observe(pic(false));
        CHECK(!alone.Routed(2));
        Model both; both.cfg.defer = true;
        for (int i = 0; i < 128; ++i) both.Observe(pic(true));
        CHECK(both.Routed(1) && both.Routed(2));
        Model oldway;
        for (int i = 0; i < 128; ++i) oldway.Observe(pic(true));
        CHECK(!oldway.Routed(2));    // round 1: 0.45 screens is under the 0.75 threshold
        // Dreadnought: nothing reaches the threshold and nothing is routed, deferred or not
        Model dread; dread.cfg.defer = true;
        std::vector<Item> d = Dread();
        for (Item& i : d) if (i.kind == IK_ROUTABLE) i.flags |= IF_DEFER;
        for (int i = 0; i < 128; ++i) dread.Observe(d);
        CHECK(dread.RoutedKeys() == 0);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

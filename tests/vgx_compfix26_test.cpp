// compfix26: host test (pure logic) of why the compositor routed nothing at Freezeflame and of the fix: the near test (vgx_fxnear.h) fed with the geometry of the Freezeflame glow keys
// (fh f9d97a8b: one draw, 3.86 screens; 97443366: 4 draws, 0.93 in all, dependent read; 6a2763ac: 0.93), the model (vgx_fxauto.h) fed with what the back end would then produce, the reject
// reason order of the stage line and GXV_FXLOWRES_FORCE. Exit status 0 when every check passes.
#include "vgx_fxauto.h"
#include "vgx_fxnear.h"

#include <cstdio>

using namespace vgx::fxauto;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// What the back end's Gather makes of a draw: IK_ROUTABLE with IF_NEAR when IsNear says so.
static Item Draw(uint32_t key, float fill, float ms, float nearW, bool autoOn, const vgx::fxnear::Rule& rule) {
    Item i;
    i.kind = IK_ROUTABLE; i.key = key; i.fill = fill; i.ms = ms; i.flags = IF_RECT; i.x0 = -1; i.y0 = -1; i.x1 = 1; i.y1 = 1;
    if (vgx::fxnear::IsNear(rule, fill, nearW, autoOn)) i.flags |= IF_NEAR;
    return i;
}
// The play camera of Freezeflame: Mario 160 units = 82 px of 544 lines at 45 degrees -> distance = 544 / (2 tan(22.5 deg) * 82/160) ~ 1280 units. Glow sheets around him: nearest vertex 700..1800.
static std::vector<Item> Picture(const vgx::fxnear::Rule& rule, bool autoOn, float jitter) {
    std::vector<Item> v;
    v.push_back(Item{});                                    // opaque world (IK_BREAK)
    v.back().kind = IK_BREAK;
    v.push_back(Draw(0xf9d97a, 3.86f * (1 + jitter), 1.5f, 1100.0f, autoOn, rule));   // the big stacked additive glow sheets: one draw
    for (int k = 0; k < 4; ++k) v.push_back(Draw(0x974433, 0.23f * (1 + jitter), 3.0f, 900.0f + 200.0f * k, autoOn, rule)); // 4 warped draws, 0.93 in all
    v.push_back(Draw(0xaaaaaa, 0.02f, 1.5f, 600.0f, autoOn, rule));                  // a spark near Mario: tiny
    v.push_back(Draw(0xbbbbbb, 0.30f, 1.5f, 300.0f, autoOn, rule));                  // a big layer ON the lens (nearest vertex 300 < 25 % of 2500): stays protected
    return v;
}

int main() {
    const vgx::fxnear::Rule oldRule{2500.0f, 0.0f, 0.25f};  // the old behaviour: NEAR_BIG=0
    const vgx::fxnear::Rule newRule{2500.0f, 0.15f, 0.25f}; // the default now
    // ---- 1. the near test itself
    CHECK(vgx::fxnear::IsNear(oldRule, 3.86f, 1100.0f, true));            // the cause: a glow sheet 1100 units away was "near"
    CHECK(!vgx::fxnear::IsNear(newRule, 3.86f, 1100.0f, true));           // now a layer
    CHECK(vgx::fxnear::IsNear(newRule, 3.86f, 1100.0f, false));           // without AUTO (the legacy path) the old rule stands
    CHECK(vgx::fxnear::IsNear(newRule, 0.02f, 1100.0f, true));            // a small sprite near the camera stays protected (star bits, sparkles)
    CHECK(vgx::fxnear::IsNear(newRule, 0.30f, 300.0f, true));             // a layer on the lens stays protected
    CHECK(!vgx::fxnear::IsNear(newRule, 0.30f, 700.0f, true));            // a layer 700 units out (>= 625) goes
    CHECK(!vgx::fxnear::IsNear(newRule, 0.01f, 3000.0f, true));           // far is far
    CHECK(vgx::fxnear::IsNear(newRule, 0.149f, 2000.0f, true) && !vgx::fxnear::IsNear(newRule, 0.15f, 2000.0f, true)); // the size edge
    // ---- 2. OLD rule: the Freezeflame glow keys are blocked, the stage line says so (and used to call it "fill under the threshold")
    {
        Model m;
        for (int p = 0; p < 400; ++p) m.Observe(Picture(oldRule, true, 0.0f));
        CHECK(m.RoutedKeys() == 0);
        uint32_t why[Model::kWhy];
        m.WindowWhy(why);
        std::printf("old rule: why %u/%u/%u/%u/%u/%u (absent / too few / fill under / mostly near-thin / budget / confirming)\n", why[0], why[1], why[2], why[3], why[4], why[5]);
        CHECK(why[3] >= 2 && why[2] == 0);  // reported as "mostly near or thin" now, not "fill under the threshold"
    }
    // ---- 3. NEW rule: the glow keys qualify, are routed after the confirming windows; the spark and the lens layer are not
    {
        Model m;
        for (int p = 0; p < 200; ++p) m.Observe(Picture(newRule, true, 0.0f));
        CHECK(m.Routed(0xf9d97a));
        CHECK(m.Routed(0x974433));          // 0.93 in all over 4 draws: the key's fill is the picture's sum
        CHECK(!m.Routed(0xaaaaaa));
        CHECK(!m.Routed(0xbbbbbb));
        Decision d[16];
        const int n = m.TakeDecisions(d, 16);
        std::printf("new rule: %d keys routed at 1/%d size\n", n, m.Div());
        CHECK(n == 2);
        // the run the back end would then take: both glow keys, one deferred or in-place run, the budget holds
        std::vector<Run> runs;
        FormRuns(Picture(newRule, true, 0.0f), [&](const Item& x) { return !(x.flags & (IF_NEAR | IF_THIN)) && m.Routed(x.key); }, runs);
        CHECK(!runs.empty());
        float net = 0.0f;
        for (const Run& r : runs) net += RunSaveMs(r, m.Div()) - RunCostMs(r, m.Div(), m.cfg);
        std::printf("new rule: %zu runs, net %.2f ms\n", runs.size(), net);
        CHECK(net > 2.0f);
    }
    // ---- 4. noisy pictures (the camera moves): still routed, once, never un-routed
    {
        Model m;
        bool was = false;
        int flips = 0;
        unsigned seed = 5;
        for (int p = 0; p < 600; ++p) {
            seed = seed * 1664525u + 1013904223u;
            const float j = (static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 0.6f;
            m.Observe(Picture(newRule, true, j));
            const bool now = m.Routed(0xf9d97a);
            if (was && !now) ++flips;
            was = now;
        }
        CHECK(was && flips == 0);
    }
    // ---- 5. GXV_FXLOWRES_FORCE: no windows, no budget; half size for 1, quarter for 2; near keys still excluded by the back end's flags
    for (int level = 1; level <= 2; ++level) {
        Model m;
        m.cfg.force = level;
        m.cfg.forceScreens = 0.05f;
        m.cfg.div = level >= 2 ? 4 : 2;
        m.Observe(Picture(newRule, true, 0.0f));          // ONE picture
        CHECK(m.Routed(0xf9d97a) && m.Routed(0x974433));
        CHECK(!m.Routed(0xaaaaaa) && !m.Routed(0xbbbbbb)); // 0.02 under the force floor; the lens layer is IF_NEAR
        CHECK(m.Div() == (level >= 2 ? 4 : 2));
    }
    {   // FORCE does not route a key whose draws are all blocked
        Model m;
        m.cfg.force = 1;
        m.Observe(Picture(oldRule, true, 0.0f));
        CHECK(m.RoutedKeys() == 0);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
